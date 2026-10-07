/*
 * ========================================================================
 * Copyright (C) 2026, Charles Chiou
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * ========================================================================
 */

/*
 * Non-interactive mail operations for MCP.  Main thread only, called
 * from mcp_drain() with mcp_noninteractive() true.  Nothing here reads
 * the keyboard, paints the screen, queues a status message, or calls
 * the human login path.
 */

#include "../headers.h"
#include "../state.h"
#include "../conf.h"
#include "../context.h"
#include "../folder.h"
#include "../stream.h"
#include "../imap.h"

#include "mcpjson.h"
#include "mcp.h"
#include "mcpint.h"
#include "mcpmail.h"


/*
 * Every mailbox string mail_list_folders has returned in this process.
 * A folder argument must be one of these.
 */
static char **known_mbox;
static int    known_n, known_cap;


typedef struct mcp_list_ctx {
    cJSON *rows;
    char  *label;
    int	   count;
} MCP_LIST_CTX_S;


/*
 * Internal prototypes
 */
static void	mcp_known_add(const char *);
static void	mcp_list_filter(MAILSTREAM *, char *, int, long, void *, unsigned);
static int	mcp_collection_pattern(CONTEXT_S *, char *, size_t);
static void	mcp_add_row(cJSON *, const char *, const char *, int, int, int);
static char    *mcp_context_label(CONTEXT_S *);


static void
mcp_known_add(const char *mbox)
{
    if(mbox == NULL || *mbox == '\0' || mcp_mailbox_known(mbox))
      return;

    if(known_n == known_cap){
	int    ncap = known_cap ? known_cap * 2 : 64;
	char **nl = (char **) fs_get(ncap * sizeof(char *));

	if(known_n)
	  memcpy(nl, known_mbox, known_n * sizeof(char *));

	if(known_mbox)
	  fs_give((void **) &known_mbox);

	known_mbox = nl;
	known_cap = ncap;
    }

    known_mbox[known_n++] = cpystr(mbox);
}


int
mcp_mailbox_known(const char *mbox)
{
    int i;

    if(mbox == NULL)
      return(0);

    for(i = 0; i < known_n; i++)
      if(!strcmp(known_mbox[i], mbox))
	return(1);

    return(0);
}


/*
 * Cache-only login, called from mm_login while a tool runs.  A
 * password the human already used on this host in this session is
 * used again.  Anything else leaves *pwd NULL, which makes c-client
 * abort the login, and records that the host needs the human.
 */
void
mcp_login_cached(NETMBX *mb, char *user, char **pwd, long trial)
{
    STRLIST_S	    hostlist[2];
    char	    hostlist0[MAILTMPLEN], hostlist1[MAILTMPLEN];
    char	    non_def_port[20], *last;
    int		    alt = (mb->sslflag || mb->tlsflag);
    struct servent *sv;

    if(pwd)
      *pwd = NULL;

    non_def_port[0] = '\0';
    if(mb->port && mb->service[0]
       && (sv = getservbyname(mb->service, "tcp"))
       && (mb->port != ntohs(sv->s_port))){
	snprintf(non_def_port, sizeof(non_def_port), ":%lu", mb->port);
	non_def_port[sizeof(non_def_port)-1] = '\0';
    }

    /* same host list mm_login_work builds, so the cache keys match */
    snprintf(hostlist0, sizeof(hostlist0), "%s%s", mb->host, non_def_port);
    hostlist[0].name = hostlist0;
    hostlist[0].next = NULL;
    if(mb->orighost[0] && strucmp(mb->host, mb->orighost)){
	snprintf(hostlist1, sizeof(hostlist1), "%s%s", mb->orighost,
		 non_def_port);
	hostlist[0].next = &hostlist[1];
	hostlist[1].name = hostlist1;
	hostlist[1].next = NULL;
    }

    /* trial > 0: the cached password was refused; do not ask anyone */
    if(trial == 0L && pwd){
	strncpy(user, (*mb->user) ? mb->user
				  : ps_global->VAR_USER_ID
				      ? ps_global->VAR_USER_ID : "",
		NETMAXUSER);
	user[NETMAXUSER-1] = '\0';

	if(*user && imap_get_passwd(mm_login_list, pwd, user, hostlist, alt))
	  return;

	if(!*mb->user && (last = imap_get_user(mm_login_list, hostlist))){
	    strncpy(user, last, NETMAXUSER);
	    user[NETMAXUSER-1] = '\0';
	    if(imap_get_passwd(mm_login_list, pwd, user, hostlist, alt))
	      return;
	}

	if(!*mb->user && ps_global->ui.login && ps_global->ui.login[0]){
	    strncpy(user, ps_global->ui.login, NETMAXUSER);
	    user[NETMAXUSER-1] = '\0';
	    if(imap_get_passwd(mm_login_list, pwd, user, hostlist, alt))
	      return;
	}
    }

    if(pwd && *pwd)
      fs_give((void **) pwd);

    mcp_note_login_needed(mb->orighost[0] ? mb->orighost : mb->host);
}


/*
 * Token logins (XOAUTH2) are not attempted from MCP.  The human logs
 * in from the Alpine screen; until then this host answers -32008.
 */
void
mcp_login_method_refused(NETMBX *mb)
{
    if(mb == NULL)
      return;

    mcp_note_login_needed(mb->orighost[0] ? mb->orighost : mb->host);
}


static char *
mcp_context_label(CONTEXT_S *c)
{
    if(c->nickname && *c->nickname)
      return(c->nickname);

    if(c->label && *c->label)
      return(c->label);

    return(c->context ? c->context : "");
}


/*
 * LIST pattern for a whole collection: its context with the folder
 * placeholder ("[]", kept internally as "%s") replaced by "*".  Built
 * from the configured context, never from the directory the human is
 * browsing.  Returns 0, or -1 if the context has no placeholder.
 */
static int
mcp_collection_pattern(CONTEXT_S *c, char *buf, size_t len)
{
    char *ctx = c->context, *from, *ph;

    if(ctx == NULL)
      return(-1);

    from = ctx;
    if(*ctx == '{' && (from = strchr(ctx, '}')) == NULL)
      return(-1);

    if((ph = strstr(from, "%s")) == NULL)
      return(-1);

    if(snprintf(buf, len, "%.*s*%s", (int)(ph - ctx), ctx, ph + 2)
       >= (int) len)
      return(-1);

    return(0);
}


static void
mcp_add_row(cJSON *rows, const char *mailbox, const char *label,
	    int incoming, int selectable, int delim)
{
    cJSON *row;
    char   d[2];

    if((row = cJSON_CreateObject()) == NULL)
      return;

    cJSON_AddStringToObject(row, "mailbox", mailbox);

    /*
     * "name" is what Alpine shows for the folder: the part after the
     * "{host}" prefix, with IMAP's modified UTF-7 (&UiqW...-) decoded to
     * UTF-8.  "mailbox" stays the exact string the other tools take.
     */
    {
	const char    *p = mailbox, *brace;
	unsigned char *dec;
	char	      *clean;

	if(*p == '{' && (brace = strchr(p, '}')) != NULL)
	  p = brace + 1;

	dec = folder_name_decoded((unsigned char *) p);
	clean = mcp_clean_utf8(dec ? (char *) dec : p,
			       strlen(dec ? (char *) dec : p), 512);
	cJSON_AddStringToObject(row, "name", clean);
	fs_give((void **) &clean);
	if(dec)
	  fs_give((void **) &dec);
    }

    cJSON_AddStringToObject(row, "collection", label ? label : "");
    cJSON_AddBoolToObject(row, "incoming", incoming);
    cJSON_AddBoolToObject(row, "selectable", selectable);
    if(delim){
	d[0] = (char) delim;
	d[1] = '\0';
	cJSON_AddStringToObject(row, "delimiter", d);
    }

    cJSON_AddItemToArray(rows, row);
    mcp_known_add(mailbox);
}


/* mm_list filter: one LIST reply line */
static void
mcp_list_filter(MAILSTREAM *stream, char *mailbox, int delim, long attrib,
		void *data, unsigned options)
{
    MCP_LIST_CTX_S *lc = (MCP_LIST_CTX_S *) data;

    (void) stream;
    (void) options;

    if(lc == NULL || mailbox == NULL || *mailbox == '\0')
      return;

    mcp_add_row(lc->rows, mailbox, lc->label, 0,
		(attrib & LATT_NOSELECT) ? 0 : 1, delim);
    lc->count++;
}


/*
 * mail_list_folders: every incoming folder from the configuration,
 * then a LIST of every folder collection.  One collection failing does
 * not hide the others; it is reported in "collections".
 */
void
mcp_run_list_folders(cJSON *args, MCP_CALL_S *call)
{
    CONTEXT_S	   *c;
    FOLDER_S	   *f;
    MM_LIST_S	    mli, *save_mli;
    MCP_LIST_CTX_S  lc;
    cJSON	   *res, *rows, *colls, *crow;
    char	    pattern[MAILTMPLEN], host[256], err[512];
    int		    i, ok, any_ok = 0, any_auth = 0, n_coll = 0;

    (void) args;

    if((res = cJSON_CreateObject()) == NULL){
	mcp_call_error(call, MCP_ERR_INTERNAL, "out of memory");
	return;
    }

    rows  = cJSON_AddArrayToObject(res, "folders");
    colls = cJSON_AddArrayToObject(res, "collections");

    for(c = ps_global->context_list; c; c = c->next){
	if(NEWS_TEST(c))		/* news groups are not mail folders */
	  continue;

	n_coll++;
	crow = cJSON_CreateObject();
	cJSON_AddStringToObject(crow, "label", mcp_context_label(c));
	cJSON_AddBoolToObject(crow, "incoming", (c->use & CNTXT_INCMNG) ? 1 : 0);

	if(c->use & CNTXT_INCMNG){
	    int count = 0;

	    /* configured list; the magic INBOX entry is inbox-path */
	    for(i = 0; c->dir && FOLDERS(c) && i < folder_total(FOLDERS(c)); i++){
		if((f = folder_entry(i, FOLDERS(c))) == NULL)
		  continue;

		if(!strucmp(f->name, ps_global->inbox_name)
		   && ps_global->VAR_INBOX_PATH)
		  mcp_add_row(rows, ps_global->VAR_INBOX_PATH,
			      mcp_context_label(c), 1, 1, 0);
		else
		  mcp_add_row(rows, f->name, mcp_context_label(c), 1, 1, 0);

		count++;
	    }

	    cJSON_AddStringToObject(crow, "status", "ok");
	    cJSON_AddNumberToObject(crow, "count", count);
	    cJSON_AddItemToArray(colls, crow);
	    any_ok++;
	    continue;
	}

	if(mcp_collection_pattern(c, pattern, sizeof(pattern)) < 0){
	    cJSON_AddStringToObject(crow, "status", "error");
	    cJSON_AddStringToObject(crow, "error", "collection has no folder placeholder");
	    cJSON_AddItemToArray(colls, crow);
	    continue;
	}

	memset(&lc, 0, sizeof(lc));
	lc.rows	 = rows;
	lc.label = mcp_context_label(c);

	memset(&mli, 0, sizeof(mli));
	mli.stream = NULL;
	mli.filter = mcp_list_filter;
	mli.data   = &lc;

	mcp_cur_reset();
	save_mli = mm_list_info;
	mm_list_info = &mli;
	ok = pine_mail_list(NULL, NULL, pattern, NULL);
	mm_list_info = save_mli;

	if(mcp_cur_login_needed(host, sizeof(host))){
	    any_auth++;
	    cJSON_AddStringToObject(crow, "status", "not_authenticated");
	    cJSON_AddNumberToObject(crow, "code", MCP_ERR_NOT_AUTH);
	    cJSON_AddStringToObject(crow, "host", host);
	}
	else if(!ok || (lc.count == 0 && mcp_cur_cclient_error(err, sizeof(err)))){
	    if(!mcp_cur_cclient_error(err, sizeof(err)))
	      snprintf(err, sizeof(err), "LIST failed");

	    cJSON_AddStringToObject(crow, "status", "error");
	    cJSON_AddNumberToObject(crow, "code", MCP_ERR_INTERNAL);
	    cJSON_AddStringToObject(crow, "error", err);
	}
	else{
	    cJSON_AddStringToObject(crow, "status", "ok");
	    any_ok++;
	}

	cJSON_AddNumberToObject(crow, "count", lc.count);
	cJSON_AddItemToArray(colls, crow);
    }

    mcp_cur_reset();

    /* nothing at all could be read: say why at the protocol level */
    if(n_coll > 0 && !any_ok){
	cJSON_Delete(res);
	if(any_auth)
	  mcp_call_error(call, MCP_ERR_NOT_AUTH,
			 "not logged in; log in on the Alpine screen");
	else
	  mcp_call_error(call, MCP_ERR_INTERNAL, "no folder collection could be listed");

	return;
    }

    snprintf(call->summary, sizeof(call->summary), "%d folders%s",
	     cJSON_GetArraySize(rows),
	     (any_ok < n_coll) ? ", some collections failed" : "");
    call->result = res;
}

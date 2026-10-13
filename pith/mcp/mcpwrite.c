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
 * Mutation tools, plan section 2.8, stage 6: mail_copy, mail_set_flag,
 * mail_create_draft.  Main thread only, called from mcp_drain() with
 * mcp_noninteractive() true.
 *
 * What these never do, by plan:
 *   - expunge, or close a stream with CL_EXPUNGE
 *   - call want_to, or any prompt
 * mail_copy files a copy; if move is true, the source is marked \Deleted
 * but never expunged.
 */

#include "../headers.h"
#include "../state.h"
#include "../conf.h"
#include "../context.h"
#include "../folder.h"
#include "../stream.h"
#include "../imap.h"
#include "../send.h"
#include "../reply.h"
#include "../pattern.h"
#include "../copyaddr.h"
#include "../store.h"

#include "mcpjson.h"
#include "mcp.h"
#include "mcpint.h"
#include "mcpconf.h"


/* base copy flags; CP_MOVE is added only when move is true */
#define MCP_COPY_FLAGS_BASE	(CP_UID)

/* compile-time check that the base copy flags never include CP_MOVE */
typedef char mcp_copy_flags_base_have_no_cp_move[((MCP_COPY_FLAGS_BASE) & (CP_MOVE)) ? -1 : 1];

#define MCP_COPY_FLAGS(move)	((MCP_COPY_FLAGS_BASE) | ((move) ? CP_MOVE : 0))

#define MCP_BODY_MAX	262144UL


static int	mcp_pending_reply(int, MCP_CALL_S *);
static long	mcp_copy_by_append(MAILSTREAM *, unsigned long, unsigned long,
				   const char *, MCP_CALL_S *);
static ACTION_S *mcp_role_find(const char *, MCP_CALL_S *);
static void	mcp_args_hash(const char *, cJSON *, char *);
static int	mcp_parse_list(ADDRESS **, const char *, const char *,
			       MCP_CALL_S *);
static const char *mcp_str_arg(cJSON *, const char *);


/*
 * Copy to a folder on another server (or between an IMAP server and a
 * local file): fetch the message without marking it seen and append it
 * to the destination with its flags, minus \\Deleted.  Needs the destination
 * host to be logged in already.  Sets call on failure; returns T or NIL.
 */
#define MCP_APPEND_MAX	(64UL * 1024UL * 1024UL)

static long
mcp_copy_by_append(MAILSTREAM *s, unsigned long msgno, unsigned long uid,
		   const char *dest, MCP_CALL_S *call)
{
    MESSAGECACHE *elt;
    STRING	  st;
    char	 *text, flags[64], date[64], seq[32], host[256], err[512];
    unsigned long len = 0;
    long	  ok;

    snprintf(seq, sizeof(seq), "%lu", uid);
    mail_fetch_fast(s, seq, FT_UID);
    elt = mail_elt(s, msgno);
    if(elt->rfc822_size > MCP_APPEND_MAX){
	mcp_call_error(call, MCP_ERR_PARAMS,
		       "message is too large to copy between servers");
	return(NIL);
    }

    text = mail_fetch_message(s, uid, &len, FT_UID | FT_PEEK);
    if(text == NULL || len == 0){
	mcp_call_error(call, MCP_ERR_INTERNAL, "could not fetch the message");
	return(NIL);
    }

    flags[0] = '\0';
    if(elt->seen)
      strcat(flags, "\\Seen ");

    if(elt->flagged)
      strcat(flags, "\\Flagged ");

    if(elt->answered)
      strcat(flags, "\\Answered ");

    if(elt->draft)
      strcat(flags, "\\Draft ");

    if(flags[0])
      flags[strlen(flags) - 1] = '\0';

    date[0] = '\0';
    if(elt->day)
      mail_date(date, elt);

    INIT(&st, mail_string, (void *) text, len);
    mcp_cur_reset();
    ok = pine_mail_append_full(NULL, (char *) dest, flags[0] ? flags : NULL,
			       date[0] ? date : NULL, &st);
    ps_global->try_to_create = 0;

    if(mcp_cur_login_needed(host, sizeof(host))){
	mcp_call_error(call, MCP_ERR_NOT_AUTH,
		       "host %.200s is not logged in; log in on the Alpine screen",
		       host);
	return(NIL);
    }

    if(!ok){
	if(!mcp_cur_cclient_error(err, sizeof(err)))
	  snprintf(err, sizeof(err), "the copy failed");

	mcp_call_error(call, MCP_ERR_INTERNAL, "%.400s", err);
	return(NIL);
    }

    return(T);
}


/*----------------------------------------------------------------------
    mail_copy: UID COPY of one message to another mailbox.  If move is
    true, marks the source \Deleted.  Never expunges.
  ----*/
void
mcp_run_copy(cJSON *args, MCP_CALL_S *call)
{
    const char	*dest;
    cJSON	*res, *f, *uarr;
    MAILSTREAM	*s;
    MESSAGECACHE *elt;
    char	 seq[32], host[256], err[512];
    unsigned long uid, msgno;
    int		 opened, move, deleted_now = 0;
    long	 ok;

    f    = cJSON_GetObjectItemCaseSensitive(args, "folder");
    dest = mcp_str_arg(args, "destination");
    move = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(args, "move"));

    if(!mcp_mailbox_known(dest)){
	snprintf(call->folder, sizeof(call->folder), "%s", f->valuestring);
	mcp_call_error(call, MCP_ERR_PARAMS,
		       "destination is not a mailbox string returned by mail_list_folders");
	return;
    }

    if((s = mcp_acquire(f->valuestring, &opened, call)) == NULL)
      return;

    uarr = cJSON_GetObjectItemCaseSensitive(args, "uids");
    if(uarr != NULL){
	MCP_UIDS_S uids;
	size_t n_req, i;
	unsigned long copied_count = 0;
	int state[MCP_UID_BATCH_MAX];
	size_t n_not_removed = 0;
	unsigned long not_removed[MCP_UID_BATCH_MAX];

	if(move && s->rdonly){
	    mcp_call_error(call, MCP_ERR_INTERNAL, "the folder is read-only");
	    mcp_release(s, opened);
	    return;
	}

	if(!mcp_uids_prepare(s, args, call, &uids, NULL, 0, NULL)){
	    mcp_release(s, opened);
	    return;
	}

	n_req = (size_t) cJSON_GetArraySize(uarr);
	mcp_cur_reset();

	if(*dest != '{' && s->mailbox && *s->mailbox != '{'){
	    /* local folder to local folder */
	    ok = pine_mail_copy_full(s, uids.set, (char *) dest, MCP_COPY_FLAGS(move));
	    copied_count = uids.n;
	}
	else if(*dest == '{' && same_stream((char *) dest, s)){
	    /*
	     * Same IMAP server: UID COPY takes the folder's name on that
	     * server, not the "{host}" form (the server answers BAD to it).
	     * Alpine's own save strips the prefix the same way.
	     */
	    const char *local = strchr(dest, '}');

	    local = (local && local[1]) ? local + 1 : "INBOX";
	    ok = pine_mail_copy_full(s, uids.set, (char *) local, MCP_COPY_FLAGS(move));
	    copied_count = uids.n;
	}
	else {
	    /* Cross-server append path */
	    if(n_req > MCP_APPEND_BATCH_MAX || uids.n > MCP_APPEND_BATCH_MAX){
		mcp_call_error(call, MCP_ERR_PARAMS,
			       "too many messages to copy between servers");
		mcp_release(s, opened);
		return;
	    }

	    mail_fetch_fast(s, uids.set, FT_UID);
	    {
		unsigned long total_size = 0;

		for(i = 0; i < uids.n; i++){
		    unsigned long m = mail_msgno(s, uids.uid[i]);

		    if(m){
			MESSAGECACHE *e = mail_elt(s, m);

			if(e)
			  total_size += e->rfc822_size;
		    }
		}

		if(total_size > MCP_APPEND_MAX){
		    mcp_call_error(call, MCP_ERR_PARAMS,
				   "messages are too large to copy between servers");
		    mcp_release(s, opened);
		    return;
		}
	    }

	    ok = 1;
	    {
		unsigned long appended_uids[MCP_APPEND_BATCH_MAX];

		for(i = 0; i < uids.n; i++){
		    unsigned long m = mail_msgno(s, uids.uid[i]);

		    if(!m){
			if(uids.nmissing < MCP_UID_BATCH_MAX){
			    size_t k, already = 0;

			    for(k = 0; k < uids.nmissing; k++){
				if(uids.missing[k] == uids.uid[i]){
				    already = 1;
				    break;
				}
			    }
			    if(!already)
			      uids.missing[uids.nmissing++] = uids.uid[i];
			}
			continue;
		    }

		    if(!mcp_copy_by_append(s, m, uids.uid[i], dest, call)){
			if(call->code != MCP_ERR_NOT_AUTH){
			    char reason[512];

			    if(call->message[0] != '\0')
			      snprintf(reason, sizeof(reason), "%s", call->message);
			    else
			      snprintf(reason, sizeof(reason), "the copy failed");

			    mcp_call_error(call, MCP_ERR_INTERNAL,
					   "copied %lu of %lu messages; failed at uid %lu: %s",
					   copied_count, (unsigned long) uids.n,
					   uids.uid[i], reason);
			}
			ok = 0;
			break;
		    }

		    appended_uids[copied_count++] = uids.uid[i];
		}

		if(move && copied_count > 0){
		    char app_set[4096];

		    if(mcp_uid_set(appended_uids, copied_count, app_set, sizeof(app_set)) > 0){
			mcp_cur_reset();
			mail_flag(s, app_set, "\\Deleted", ST_UID | ST_SET);
		    }
		}
	    }
	}

	if(call->code != 0){
	    mcp_release(s, opened);
	    return;
	}

	if(mcp_cur_login_needed(host, sizeof(host))){
	    mcp_call_error(call, MCP_ERR_NOT_AUTH,
			   "host %.200s is not logged in; log in on the Alpine screen",
			   host);
	    mcp_release(s, opened);
	    return;
	}

	if(!ok){
	    if(!mcp_cur_cclient_error(err, sizeof(err)))
	      snprintf(err, sizeof(err), "the copy failed");

	    if(strlen(uids.set) > 1000)
	      mcp_call_error(call, MCP_ERR_INTERNAL,
			     "%.350s (some messages may already have been copied)", err);
	    else
	      mcp_call_error(call, MCP_ERR_INTERNAL, "%.400s", err);

	    mcp_release(s, opened);
	    return;
	}

	/* move read-back (before release) */
	memset(state, 0, sizeof(state));
	if(move){
	    if(mcp_uids_prepare(s, args, call, &uids, "\\Deleted", 1, state)){
		unsigned long differ_uids[MCP_UID_BATCH_MAX];
		size_t nd = 0;

		for(i = 0; i < n_req; i++){
		    if(state[i] == MCP_UID_DIFFER){
			cJSON *it = cJSON_GetArrayItem(uarr, (int) i);
			differ_uids[nd++] = (unsigned long) it->valuedouble;
		    }
		}

		if(nd > 0){
		    char differ_set[4096];

		    if(mcp_uid_set(differ_uids, nd, differ_set, sizeof(differ_set)) > 0){
			mcp_cur_reset();
			mail_flag(s, differ_set, "\\Deleted", ST_UID | ST_SET);
		    }
		    mcp_uids_prepare(s, args, call, &uids, "\\Deleted", 1, state);
		}

		deleted_now = 1;
		for(i = 0; i < n_req; i++){
		    if(state[i] == MCP_UID_DIFFER){
			deleted_now = 0;
			cJSON *it = cJSON_GetArrayItem(uarr, (int) i);
			not_removed[n_not_removed++] = (unsigned long) it->valuedouble;
		    }
		}
	    }
	}

	ps_global->try_to_create = 0;
	mcp_release(s, opened);

	if((res = cJSON_CreateObject()) == NULL){
	    mcp_call_error(call, MCP_ERR_INTERNAL, "out of memory");
	    return;
	}

	cJSON_AddBoolToObject(res, "copied", 1);
	cJSON_AddBoolToObject(res, "source_removed", deleted_now);
	cJSON_AddStringToObject(res, "source", f->valuestring);
	cJSON_AddItemToObject(res, "uids", cJSON_Duplicate(uarr, 1));
	cJSON_AddNumberToObject(res, "count", (double) copied_count);
	cJSON_AddStringToObject(res, "destination", dest);

	if(uids.nmissing > 0 || (move && deleted_now == 0)){
	    /* Populate missing_uids */
	    cJSON *missing_arr = NULL;

	    for(i = 0; i < n_req; i++){
		int is_missing = 0;
		cJSON *it = cJSON_GetArrayItem(uarr, (int) i);
		unsigned long req_u = (unsigned long) it->valuedouble;

		if(move && state[i] == MCP_UID_GONE)
		  is_missing = 1;
		else {
		    size_t k;

		    for(k = 0; k < uids.nmissing; k++){
			if(uids.missing[k] == req_u){
			    is_missing = 1;
			    break;
			}
		    }
		}

		if(is_missing){
		    if(missing_arr == NULL)
		      missing_arr = cJSON_CreateArray();
		    cJSON_AddItemToArray(missing_arr, cJSON_CreateNumber(req_u));
		}
	    }

	    if(missing_arr != NULL)
	      cJSON_AddItemToObject(res, "missing_uids", missing_arr);
	}

	if(move && !deleted_now && n_not_removed > 0){
	    cJSON *nr_arr = cJSON_CreateArray();

	    for(i = 0; i < n_not_removed; i++)
	      cJSON_AddItemToArray(nr_arr, cJSON_CreateNumber(not_removed[i]));

	    cJSON_AddItemToObject(res, "not_removed_uids", nr_arr);
	}

	if(move)
	  snprintf(call->summary, sizeof(call->summary),
		   "moved %lu messages (sources marked \\Deleted)", copied_count);
	else
	  snprintf(call->summary, sizeof(call->summary),
		   "copied %lu messages", copied_count);

	call->result = res;
	return;
    }

    if(!mcp_uid_ok(s, args, call, &uid)){
	mcp_release(s, opened);
	return;
    }

    if(move && s->rdonly){
	mcp_call_error(call, MCP_ERR_INTERNAL, "the folder is read-only");
	mcp_release(s, opened);
	return;
    }

    if((msgno = mail_msgno(s, uid)) == 0L){
	mcp_call_error(call, MCP_ERR_PARAMS, "no message with that uid");
	mcp_release(s, opened);
	return;
    }

    snprintf(seq, sizeof(seq), "%lu", uid);
    mcp_cur_reset();

    if(*dest != '{' && s->mailbox && *s->mailbox != '{'){
	/* local folder to local folder */
	ok = pine_mail_copy_full(s, seq, (char *) dest, MCP_COPY_FLAGS(move));
    }
    else if(*dest == '{' && same_stream((char *) dest, s)){
	/*
	 * Same IMAP server: UID COPY takes the folder's name on that
	 * server, not the "{host}" form (the server answers BAD to it).
	 * Alpine's own save strips the prefix the same way.
	 */
	const char *local = strchr(dest, '}');

	local = (local && local[1]) ? local + 1 : "INBOX";
	ok = pine_mail_copy_full(s, seq, (char *) local, MCP_COPY_FLAGS(move));
    }
    else {
	ok = mcp_copy_by_append(s, msgno, uid, dest, call);
	if(ok && move)
	  mail_flag(s, seq, "\\Deleted", ST_UID | ST_SET);
    }

    if(call->code != 0){
	mcp_release(s, opened);
	return;
    }

    if(mcp_cur_login_needed(host, sizeof(host))){
	mcp_call_error(call, MCP_ERR_NOT_AUTH,
		       "host %.200s is not logged in; log in on the Alpine screen",
		       host);
	mcp_release(s, opened);
	return;
    }

    if(!ok){
	if(!mcp_cur_cclient_error(err, sizeof(err)))
	  snprintf(err, sizeof(err), "the copy failed");

	mcp_call_error(call, MCP_ERR_INTERNAL, "%.400s", err);
	mcp_release(s, opened);
	return;
    }

    /*
     * Verify, do not assume: read back elt->deleted before releasing the stream.
     */
    if(move){
	mail_fetch_flags(s, seq, FT_UID);
	elt = mail_elt(s, msgno);
	deleted_now = (elt && elt->deleted) ? 1 : 0;
    }

    /* a missing destination folder must not steer the human's next save */
    ps_global->try_to_create = 0;

    mcp_release(s, opened);

    if((res = cJSON_CreateObject()) == NULL){
	mcp_call_error(call, MCP_ERR_INTERNAL, "out of memory");
	return;
    }

    cJSON_AddBoolToObject(res, "copied", 1);
    cJSON_AddBoolToObject(res, "source_removed", deleted_now);
    cJSON_AddStringToObject(res, "source", f->valuestring);
    cJSON_AddNumberToObject(res, "uid", (double) uid);
    cJSON_AddStringToObject(res, "destination", dest);
    if(move)
      snprintf(call->summary, sizeof(call->summary),
	       "moved uid %lu (source marked \\Deleted)", uid);
    else
      snprintf(call->summary, sizeof(call->summary), "copied uid %lu", uid);
    call->result = res;
}


/*----------------------------------------------------------------------
    mail_set_flag: \Seen, \Flagged, \Answered, or \Deleted on one message.
    Never expunges.
  ----*/
void
mcp_run_set_flag(cJSON *args, MCP_CALL_S *call)
{
    cJSON	 *f, *res, *uarr;
    const char	 *flag;
    MAILSTREAM	 *s;
    MESSAGECACHE *elt;
    char	  seq[32];
    unsigned long uid, msgno;
    int		  opened, set, now = 0;

    f    = cJSON_GetObjectItemCaseSensitive(args, "folder");
    flag = mcp_str_arg(args, "flag");
    set  = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(args, "set"));

    if(strcmp(flag, "\\Seen") && strcmp(flag, "\\Flagged")
       && strcmp(flag, "\\Answered") && strcmp(flag, "\\Deleted")){
	snprintf(call->folder, sizeof(call->folder), "%s", f->valuestring);
	mcp_call_error(call, MCP_ERR_PARAMS,
		       "flag must be \\Seen, \\Flagged, \\Answered or \\Deleted");
	return;
    }

    if((s = mcp_acquire(f->valuestring, &opened, call)) == NULL)
      return;

    uarr = cJSON_GetObjectItemCaseSensitive(args, "uids");
    if(uarr != NULL){
	MCP_UIDS_S uids;
	int state[MCP_UID_BATCH_MAX];
	size_t n_req, ngone = 0, nmatch = 0, ndiffer = 0, i;
	int act;

	if(s->rdonly){
	    mcp_call_error(call, MCP_ERR_INTERNAL, "the folder is read-only");
	    mcp_release(s, opened);
	    return;
	}

	if(!mcp_uids_prepare(s, args, call, &uids, NULL, 0, NULL)){
	    mcp_release(s, opened);
	    return;
	}

	n_req = (size_t) cJSON_GetArraySize(uarr);

	mcp_cur_reset();
	mail_flag(s, uids.set, (char *) flag, ST_UID | (set ? ST_SET : 0));

	/* read back and mcp_batch_classify pass 1 */
	if(!mcp_uids_prepare(s, args, call, &uids, flag, set, state)){
	    mcp_release(s, opened);
	    return;
	}

	act = mcp_batch_classify(state, n_req, 1, &ngone, &nmatch, &ndiffer);
	if(act == MCP_ACT_REISSUE){
	    unsigned long differ_uids[MCP_UID_BATCH_MAX];
	    char differ_set[4096];
	    size_t nd = 0;

	    for(i = 0; i < n_req; i++){
		if(state[i] == MCP_UID_DIFFER){
		    cJSON *it = cJSON_GetArrayItem(uarr, (int) i);
		    differ_uids[nd++] = (unsigned long) it->valuedouble;
		}
	    }

	    if(nd > 0 && mcp_uid_set(differ_uids, nd, differ_set, sizeof(differ_set)) > 0){
		mcp_cur_reset();
		mail_flag(s, differ_set, (char *) flag, ST_UID | (set ? ST_SET : 0));
	    }

	    /* read back and mcp_batch_classify pass 2 */
	    if(!mcp_uids_prepare(s, args, call, &uids, flag, set, state)){
		mcp_release(s, opened);
		return;
	    }

	    act = mcp_batch_classify(state, n_req, 2, &ngone, &nmatch, &ndiffer);
	}

	if(act == MCP_ACT_FAIL){
	    unsigned long first_u = 0;

	    for(i = 0; i < n_req; i++){
		if(state[i] == MCP_UID_DIFFER){
		    cJSON *it = cJSON_GetArrayItem(uarr, (int) i);
		    first_u = (unsigned long) it->valuedouble;
		    break;
		}
	    }

	    mcp_call_error(call, MCP_ERR_INTERNAL,
			   "the server did not change the flag for %lu of %lu messages (first uid %lu)",
			   (unsigned long) ndiffer,
			   (unsigned long) (nmatch + ndiffer),
			   first_u);
	    mcp_release(s, opened);
	    return;
	}

	mcp_release(s, opened);

	if((res = cJSON_CreateObject()) == NULL){
	    mcp_call_error(call, MCP_ERR_INTERNAL, "out of memory");
	    return;
	}

	cJSON_AddStringToObject(res, "mailbox", f->valuestring);
	cJSON_AddItemToObject(res, "uids", cJSON_Duplicate(uarr, 1));
	cJSON_AddNumberToObject(res, "count", (double) nmatch);
	cJSON_AddStringToObject(res, "flag", flag);
	cJSON_AddBoolToObject(res, "set", set);

	if(ngone > 0){
	    cJSON *missing_arr = cJSON_CreateArray();

	    for(i = 0; i < n_req; i++){
		if(state[i] == MCP_UID_GONE){
		    cJSON *it = cJSON_GetArrayItem(uarr, (int) i);
		    cJSON_AddItemToArray(missing_arr, cJSON_CreateNumber(it->valuedouble));
		}
	    }
	    cJSON_AddItemToObject(res, "missing_uids", missing_arr);
	}

	snprintf(call->summary, sizeof(call->summary), "%s %s on %lu messages",
		 flag, set ? "set" : "cleared", (unsigned long) nmatch);
	call->result = res;
	return;
    }

    if(!mcp_uid_ok(s, args, call, &uid)){
	mcp_release(s, opened);
	return;
    }

    if(s->rdonly){
	mcp_call_error(call, MCP_ERR_INTERNAL, "the folder is read-only");
	mcp_release(s, opened);
	return;
    }

    if((msgno = mail_msgno(s, uid)) == 0L){
	mcp_call_error(call, MCP_ERR_PARAMS, "no message with that uid");
	mcp_release(s, opened);
	return;
    }

    snprintf(seq, sizeof(seq), "%lu", uid);
    mcp_cur_reset();
    mail_flag(s, seq, (char *) flag, ST_UID | (set ? ST_SET : 0));

    /* read the flag back; the server's answer is what counts */
    elt = mail_elt(s, msgno);
    if(!strcmp(flag, "\\Seen"))
      now = elt->seen;
    else if(!strcmp(flag, "\\Flagged"))
      now = elt->flagged;
    else if(!strcmp(flag, "\\Deleted"))
      now = elt->deleted;
    else
      now = elt->answered;

    if((now ? 1 : 0) != (set ? 1 : 0)){
	mcp_call_error(call, MCP_ERR_INTERNAL,
		       "the server did not change the flag");
	mcp_release(s, opened);
	return;
    }

    mcp_release(s, opened);

    if((res = cJSON_CreateObject()) == NULL){
	mcp_call_error(call, MCP_ERR_INTERNAL, "out of memory");
	return;
    }

    cJSON_AddStringToObject(res, "mailbox", f->valuestring);
    cJSON_AddNumberToObject(res, "uid", (double) uid);
    cJSON_AddStringToObject(res, "flag", flag);
    cJSON_AddBoolToObject(res, "set", now);
    snprintf(call->summary, sizeof(call->summary), "%s %s", flag,
	     set ? "set" : "cleared");
    call->result = res;
}


/*----------------------------------------------------------------------
    Shared with mail_send (stage 7).
  ----*/
static const char *
mcp_str_arg(cJSON *args, const char *name)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(args, name);

    return(cJSON_IsString(v) ? v->valuestring : "");
}


/*
 * A role usable by "from": its compose action is "Without confirmation".
 * Returns a copy of the role's action (caller frees with free_action),
 * or NULL with call set to -32602.  The role selected in the TUI is not
 * changed and no role menu is run.
 */
static ACTION_S *
mcp_role_find(const char *nick, MCP_CALL_S *call)
{
    PAT_STATE pstate;
    PAT_S    *pat;
    int	      any = 0;

    if(!nonempty_patterns(ROLE_COMPOSE | ROLE_DO_ROLES, &pstate)){
	mcp_call_error(call, MCP_ERR_PARAMS, "from: no such role");
	return(NULL);
    }

    for(pat = first_pattern(&pstate); pat; pat = next_pattern(&pstate)){
	if(!pat->action || !pat->action->is_a_role || !pat->action->nick
	   || strucmp(pat->action->nick, (char *) nick))
	  continue;

	any = 1;
	if(pat->action->comp_type != ROLE_COMP_NOCONF){
	    mcp_call_error(call, MCP_ERR_PARAMS,
			   "from: that role's compose action is not \"Without confirmation\"");
	    return(NULL);
	}

	if(!pat->action->from){
	    mcp_call_error(call, MCP_ERR_PARAMS, "from: that role sets no From");
	    return(NULL);
	}

	return(copy_action(pat->action));
    }

    if(!any)
      mcp_call_error(call, MCP_ERR_PARAMS, "from: no such role");

    return(NULL);
}


/* parse an address list into *lst; "" is an empty list */
static int
mcp_parse_list(ADDRESS **lst, const char *text, const char *what,
	       MCP_CALL_S *call)
{
    char    *copy;
    ADDRESS *a;

    *lst = NULL;
    if(text == NULL || *text == '\0')
      return(0);

    copy = cpystr(text);
    rfc822_parse_adrlist(lst, copy, ps_global->maildomain);
    fs_give((void **) &copy);

    if(*lst == NULL){
	mcp_call_error(call, MCP_ERR_PARAMS, "%s is not a valid address list",
		       what);
	return(-1);
    }

    for(a = *lst; a; a = a->next)
      if(a->host && (!strcmp(a->host, ".SYNTAX-ERROR.")
		     || !strcmp(a->host, ".MISSING-HOST-NAME."))){
	  mail_free_address(lst);
	  mcp_call_error(call, MCP_ERR_PARAMS,
			 "%s is not a valid address list", what);
	  return(-1);
      }

    return(0);
}


/* canonical text of the arguments, hashed for the idempotency record */
static void
mcp_args_hash(const char *op, cJSON *args, char *hex)
{
    static const char *names[] = { "from", "to", "cc", "bcc", "subject",
				   "body", NULL };
    size_t  len = strlen(op) + 1;
    char   *buf, *p;
    int	    i;

    for(i = 0; names[i]; i++)
      len += strlen(mcp_str_arg(args, names[i])) + 1 + strlen(names[i]) + 1;

    buf = (char *) fs_get(len + 1);
    p = buf;
    memcpy(p, op, strlen(op) + 1);
    p += strlen(op) + 1;
    for(i = 0; names[i]; i++){
	/* name, NUL, value, NUL: no value can run into the next name */
	memcpy(p, names[i], strlen(names[i]) + 1);
	p += strlen(names[i]) + 1;
	memcpy(p, mcp_str_arg(args, names[i]), strlen(mcp_str_arg(args, names[i])) + 1);
	p += strlen(mcp_str_arg(args, names[i])) + 1;
    }

    mcp_sha256_hex(buf, (size_t) (p - buf), hex);
    fs_give((void **) &buf);
}


/*
 * 0 and *replay NULL: go ahead, a pending record is on disk.  0 and
 * *replay set: this is a retry of a finished call; answer with it.
 * -1: call holds the error.
 */
static int
mcp_pending_reply(int rc, MCP_CALL_S *call)
{
    switch(rc){
      case MCP_IDEM_PENDING:
	mcp_call_error(call, MCP_ERR_IDEMPOTENCY,
		       "that idempotency_key is pending from an earlier attempt that did not finish; remove it on the monitor screen");
	return(-1);

      case MCP_IDEM_MISMATCH:
	mcp_call_error(call, MCP_ERR_IDEMPOTENCY,
		       "that idempotency_key was used with different arguments");
	return(-1);

      case MCP_IDEM_ERROR:
	mcp_call_error(call, MCP_ERR_INTERNAL,
		       "could not use the idempotency store");
	return(-1);
    }

    return(0);
}


/*----------------------------------------------------------------------
    Building the outgoing message, shared by mail_create_draft and
    mail_send.
  ----*/
typedef struct mcp_msg {
    ENVELOPE *env;
    BODY     *body;
    ACTION_S *role;
} MCP_MSG_S;


static void
mcp_msg_free(MCP_MSG_S *m)
{
    if(m->env)
      mail_free_envelope(&m->env);

    if(m->body)
      pine_free_body(&m->body);

    if(m->role)
      free_action(&m->role);
}


/* body text with CR removed, Alpine keeps text with LF line ends */
static char *
mcp_lf_text(const char *text)
{
    char *b = cpystr(text), *r, *w;

    for(r = w = b; *r; r++)
      if(*r != '\r')
	*w++ = *r;

    *w = '\0';
    return(b);
}


/*
 * Parse the recipients and look up the role.  Nothing is built yet, so a
 * failure leaves nothing to undo.  m->env holds only the address lists.
 */
static int
mcp_msg_parse(cJSON *args, MCP_MSG_S *m, MCP_CALL_S *call, int need_to)
{
    const char *fromnick = mcp_str_arg(args, "from");

    memset(m, 0, sizeof(*m));
    if(*fromnick && (m->role = mcp_role_find(fromnick, call)) == NULL)
      return(-1);

    m->env = mail_newenvelope();
    if(mcp_parse_list(&m->env->to, mcp_str_arg(args, "to"), "to", call) < 0
       || mcp_parse_list(&m->env->cc, mcp_str_arg(args, "cc"), "cc", call) < 0
       || mcp_parse_list(&m->env->bcc, mcp_str_arg(args, "bcc"), "bcc", call) < 0){
	mcp_msg_free(m);
	return(-1);
    }

    if(need_to && m->env->to == NULL){
	mcp_msg_free(m);
	mcp_call_error(call, MCP_ERR_PARAMS, "to is required");
	return(-1);
    }

    return(0);
}


/*
 * Fill in From, Subject, Date, Message-ID and the text body.  Returns 0,
 * or -1 with call set to -32603.
 */
static int
mcp_msg_finish(cJSON *args, MCP_MSG_S *m, MCP_CALL_S *call)
{
    STORE_S *so;
    char     date[128], *b;

    m->env->from = m->role ? rfc822_cpy_adr(m->role->from) : generate_from();

    /*
     * The envelope sender, taken from the From that will be used, as the
     * composer does (alpine/send.c).  Without it c-client sends
     * MAIL FROM:<>, which receiving servers treat as a bounce and often
     * drop without a word.
     */
    if(m->env->return_path)
      mail_free_address(&m->env->return_path);

    m->env->return_path = rfc822_cpy_adr(m->env->from);
    m->env->subject = cpystr(mcp_str_arg(args, "subject"));
    rfc822_date(date);
    m->env->date = (unsigned char *) cpystr(date);
    m->env->message_id = generate_message_id(m->role);

    m->body = mail_newbody();
    if((so = so_get(CharStar, NULL, EDIT_ACCESS)) == NULL){
	mcp_call_error(call, MCP_ERR_INTERNAL, "out of memory");
	return(-1);
    }

    b = mcp_lf_text(mcp_str_arg(args, "body"));
    so_puts(so, b);
    fs_give((void **) &b);
    m->body->type = TYPETEXT;
    m->body->encoding = ENCOTHER;	/* let the grope below decide */
    m->body->contents.text.data = (void *) so;
    m->body->size.bytes = (unsigned long) strlen((char *) so_text(so));

    /*
     * Same preparation the composer gives a body it hands to the mailer:
     * sniff the text for its encoding and charset, and mark it as UTF-8
     * text we edited, so that pine_encode_body labels and converts it.
     */
    set_mime_type_by_grope(m->body);
    (void) so_attr(so, "edited", "1");
    return(0);
}


/* nonzero if the body has a byte outside ASCII */
static int
mcp_body_is_wide(cJSON *args)
{
    const unsigned char *c;

    for(c = (const unsigned char *) mcp_str_arg(args, "body"); *c; c++)
      if(*c >= 0x80)
	return(1);

    return(0);
}


/*----------------------------------------------------------------------
    mail_create_draft: write one message to the configured
    postponed-folder through write_postponed.  Never asks anyone and
    never compares recipients with the allow list.
  ----*/
void
mcp_run_create_draft(cJSON *args, MCP_CALL_S *call)
{
    const char *key = mcp_str_arg(args, "idempotency_key");
    MCP_MSG_S   m;
    METAENV    *header;
    char       *fcc = NULL, **tobuf = NULL, hash[65], *stored = NULL;
    char	err[512];
    cJSON      *res, *prev;
    int		rc;
    unsigned	old_utf8;

    if(!ps_global->VAR_POSTPONED_FOLDER || !ps_global->VAR_POSTPONED_FOLDER[0]){
	mcp_call_error(call, MCP_ERR_INTERNAL, "no postponed-folder is configured");
	return;
    }

    if(mcp_msg_parse(args, &m, call, 0) < 0)
      return;

    mcp_args_hash("mail_create_draft", args, hash);
    rc = mcp_idem_begin("mail_create_draft", key, hash, &stored);
    if(rc == MCP_IDEM_DONE){
	prev = mcp_json_parse(stored, strlen(stored));
	free(stored);
	mcp_msg_free(&m);
	if(prev == NULL){
	    mcp_call_error(call, MCP_ERR_INTERNAL, "the stored result is unreadable");
	    return;
	}

	cJSON_AddBoolToObject(prev, "replayed", 1);
	snprintf(call->summary, sizeof(call->summary), "replayed");
	call->result = prev;
	return;
    }

    if(rc != MCP_IDEM_NEW){
	mcp_pending_reply(rc, call);
	mcp_msg_free(&m);
	return;
    }

    if(mcp_msg_finish(args, &m, call) < 0){
	mcp_idem_abort(key);
	mcp_msg_free(&m);
	return;
    }

    header = pine_new_env(m.env, &fcc, &tobuf, NULL);

    /*
     * Text that is not plain ASCII is written as UTF-8 and labelled so.
     * Alpine picks the posting charset from the locale it runs in; in the
     * C locale that would label UTF-8 bytes as US-ASCII.  The flag is
     * put back at once.
     */
    mcp_cur_reset();
    old_utf8 = ps_global->post_utf8;
    if(mcp_body_is_wide(args))
      ps_global->post_utf8 = 1;

    rc = write_postponed(header, m.body);
    ps_global->post_utf8 = old_utf8;

    pine_free_env(&header);
    mcp_msg_free(&m);

    if(rc < 0){
	/* nothing was written: let the caller try again with this key */
	mcp_idem_abort(key);
	if(!mcp_cur_cclient_error(err, sizeof(err)))
	  snprintf(err, sizeof(err), "could not write to the postponed folder");

	mcp_call_error(call, MCP_ERR_INTERNAL, "%.400s", err);
	return;
    }

    res = cJSON_CreateObject();
    cJSON_AddBoolToObject(res, "drafted", 1);
    cJSON_AddStringToObject(res, "folder", ps_global->VAR_POSTPONED_FOLDER);
    cJSON_AddStringToObject(res, "idempotency_key", key);
    stored = cJSON_PrintUnformatted(res);
    if(stored == NULL || mcp_idem_commit("mail_create_draft", key, hash, stored) < 0){
	/* the draft exists; the record stays pending so a retry cannot
	   write a second copy */
	if(stored)
	  cJSON_free(stored);

	cJSON_Delete(res);
	mcp_call_error(call, MCP_ERR_INTERNAL,
		       "the draft was written but its record could not be finished");
	return;
    }

    cJSON_free(stored);
    snprintf(call->summary, sizeof(call->summary), "draft written");
    call->result = res;
}


/*----------------------------------------------------------------------
    mail_send, plan sections 2.8 and 2.9.

    The operator lock was checked before the call was queued.  Here:
      1. a repeat of a finished send is answered from its record;
      2. every recipient (to, cc, bcc) on the allow list: send now;
      3. otherwise park the call; the human is asked once, from
         Alpine's command loop (mcp_confirm_pending).  A yes runs
         mcp_run_send_confirmed.  The answer is not remembered: the
         address is not added to the allow list.
    Nothing is written to the idempotency store until the send is
    actually going to be made.
  ----*/
static int
mcp_all_allowed(MCP_MSG_S *m, char *offlist, size_t len, int *noff)
{
    ADDRESS *lists[3], *a;
    char     addr[600];
    size_t   used = 0;
    int	     i, off = 0;

    lists[0] = m->env->to;
    lists[1] = m->env->cc;
    lists[2] = m->env->bcc;
    offlist[0] = '\0';
    for(i = 0; i < 3; i++)
      for(a = lists[i]; a; a = a->next){
	  if(!a->mailbox || !a->host)
	    continue;

	  snprintf(addr, sizeof(addr), "%s@%s", a->mailbox, a->host);
	  if(mcp_recipient_allowed(addr))
	    continue;

	  if(off < 2 && used + strlen(addr) + 3 < len){
	      used += (size_t) snprintf(offlist + used, len - used, "%s%s",
					off ? ", " : "", addr);
	  }

	  off++;
      }

    *noff = off;
    return(off == 0);
}


/* the actual send; the human has agreed, or no agreement is needed */
static void
mcp_send_now(cJSON *args, MCP_CALL_S *call)
{
    const char *key = mcp_str_arg(args, "idempotency_key");
    MCP_MSG_S   m;
    METAENV    *header;
    char       *fcc = NULL, **tobuf = NULL, hash[65], *stored = NULL;
    char	err[512], host[256];
    cJSON      *res, *prev;
    int		rc, nrcpt = 0;
    unsigned	old_utf8;
    ADDRESS    *a;

    if(mcp_msg_parse(args, &m, call, 1) < 0)
      return;

    mcp_args_hash("mail_send", args, hash);
    rc = mcp_idem_begin("mail_send", key, hash, &stored);
    if(rc == MCP_IDEM_DONE){
	prev = mcp_json_parse(stored, strlen(stored));
	free(stored);
	mcp_msg_free(&m);
	if(prev == NULL){
	    mcp_call_error(call, MCP_ERR_INTERNAL, "the stored result is unreadable");
	    return;
	}

	cJSON_AddBoolToObject(prev, "replayed", 1);
	snprintf(call->summary, sizeof(call->summary), "replayed");
	call->result = prev;
	return;
    }

    if(rc != MCP_IDEM_NEW){
	mcp_pending_reply(rc, call);
	mcp_msg_free(&m);
	return;
    }

    for(a = m.env->to; a; a = a->next)
      nrcpt++;

    for(a = m.env->cc; a; a = a->next)
      nrcpt++;

    for(a = m.env->bcc; a; a = a->next)
      nrcpt++;

    if(mcp_msg_finish(args, &m, call) < 0){
	mcp_idem_abort(key);
	mcp_msg_free(&m);
	return;
    }

    header = pine_new_env(m.env, &fcc, &tobuf, NULL);

    mcp_cur_reset();
    old_utf8 = ps_global->post_utf8;
    if(mcp_body_is_wide(args))
      ps_global->post_utf8 = 1;

    /* the role's SMTP server for this call only; else the primary one */
    rc = call_mailer(header, m.body, m.role ? m.role->smtp : NULL, CM_NONE,
		     NULL, NULL);
    ps_global->post_utf8 = old_utf8;

    pine_free_env(&header);
    mcp_msg_free(&m);

    if(rc != 1){
	/*
	 * Not sent.  A login that is missing is reported as such.  A failure
	 * after the mail may have left the building is not retried silently:
	 * the record is removed only when call_mailer says plainly it failed.
	 */
	mcp_idem_abort(key);
	if(mcp_cur_login_needed(host, sizeof(host))){
	    mcp_call_error(call, MCP_ERR_NOT_AUTH,
			   "host %.200s is not logged in; log in on the Alpine screen",
			   host);
	    return;
	}

	if(!mcp_cur_cclient_error(err, sizeof(err)))
	  snprintf(err, sizeof(err), "the mail was not sent");

	mcp_call_error(call, MCP_ERR_INTERNAL, "%.400s", err);
	return;
    }

    res = cJSON_CreateObject();
    cJSON_AddBoolToObject(res, "sent", 1);
    cJSON_AddNumberToObject(res, "recipients", nrcpt);
    cJSON_AddStringToObject(res, "idempotency_key", key);
    stored = cJSON_PrintUnformatted(res);
    if(stored == NULL || mcp_idem_commit("mail_send", key, hash, stored) < 0){
	/* it was sent; the record stays pending so a retry cannot send twice */
	if(stored)
	  cJSON_free(stored);

	cJSON_Delete(res);
	mcp_call_error(call, MCP_ERR_INTERNAL,
		       "the mail was sent but its record could not be finished");
	return;
    }

    cJSON_free(stored);
    snprintf(call->summary, sizeof(call->summary), "sent to %d", nrcpt);
    call->result = res;
}


void
mcp_run_send_confirmed(cJSON *args, MCP_CALL_S *call)
{
    mcp_send_now(args, call);
}


void
mcp_run_send(cJSON *args, MCP_CALL_S *call)
{
    const char *key = mcp_str_arg(args, "idempotency_key");
    MCP_MSG_S   m;
    char	hash[65], *stored = NULL, off[512], who[600];
    int		rc, noff = 0, ok;
    cJSON      *prev;

    if(mcp_msg_parse(args, &m, call, 1) < 0)
      return;

    mcp_args_hash("mail_send", args, hash);

    /* a repeat of a finished send never asks again and never sends again */
    rc = mcp_idem_check("mail_send", key, hash, &stored);
    if(rc == MCP_IDEM_DONE){
	prev = mcp_json_parse(stored, strlen(stored));
	free(stored);
	mcp_msg_free(&m);
	if(prev == NULL){
	    mcp_call_error(call, MCP_ERR_INTERNAL, "the stored result is unreadable");
	    return;
	}

	cJSON_AddBoolToObject(prev, "replayed", 1);
	snprintf(call->summary, sizeof(call->summary), "replayed");
	call->result = prev;
	return;
    }

    if(rc == MCP_IDEM_PENDING || rc == MCP_IDEM_MISMATCH || rc == MCP_IDEM_ERROR){
	mcp_msg_free(&m);
	mcp_pending_reply(rc, call);
	return;
    }

    ok = mcp_all_allowed(&m, off, sizeof(off), &noff);
    snprintf(who, sizeof(who), "-> %s%s", off[0] ? off : "recipients",
	     noff > 2 ? " ..." : "");
    mcp_msg_free(&m);

    if(ok){
	mcp_send_now(args, call);
	return;
    }

    /* off the list: ask once, from the command loop */
    snprintf(call->folder, sizeof(call->folder), "%.250s", who + 3);
    if(noff > 2)
      snprintf(call->park_prompt, sizeof(call->park_prompt),
	       "Let the agent send mail to %s and %d more (not on the allow list)",
	       off, noff - 2);
    else
      snprintf(call->park_prompt, sizeof(call->park_prompt),
	       "Let the agent send mail to %s (not on the allow list)", off);

    call->park = 1;
}

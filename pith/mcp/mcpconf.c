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
 * MCP policy file.  This file uses only libc and cJSON so the listener
 * and aimon threads may call the getters without touching c-client or
 * pith, and so a small driver can link it on its own.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "mcpjson.h"
#include "mcpconf.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define MCP_CONF_MAX_BYTES	(1024 * 1024)


static MCP_CONFIG_S    mcp_conf;		/* zero == defaults */
static pthread_mutex_t mcp_conf_mutex = PTHREAD_MUTEX_INITIALIZER;
static char	       mcp_conf_dir[PATH_MAX];
static char	       mcp_conf_path[PATH_MAX];


/*
 * Internal prototypes
 */
static char   *mcp_strdup(const char *);
static void    mcp_conf_set_error(MCP_CONFIG_S *, const char *, ...);
static void    mcp_conf_clear(MCP_CONFIG_S *);
static int     mcp_conf_parse(MCP_CONFIG_S *, cJSON *);
static char   *mcp_allow_normalize(const char *);
static int     mcp_domain_valid(const char *);
static int     valid_host(const char *);
static int     mcp_local_valid(const char *, size_t);
static char  **mcp_list_dup(char **);
static int     mcp_write_all(int, const char *, size_t);


static char *
mcp_strdup(const char *s)
{
    size_t  len;
    char   *d;

    if(s == NULL)
      return(NULL);

    len = strlen(s);
    if((d = malloc(len + 1)) == NULL)
      return(NULL);

    memcpy(d, s, len + 1);
    return(d);
}


static void
mcp_conf_set_error(MCP_CONFIG_S *c, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(c->last_error, sizeof(c->last_error), fmt, ap);
    va_end(ap);
}


/*
 * Free what c owns and reset it to the fail-closed defaults.
 * last_error is kept.
 */
static void
mcp_conf_clear(MCP_CONFIG_S *c)
{
    mcp_free_list(&c->allowed);
    if(c->aimon_host){
	free(c->aimon_host);
	c->aimon_host = NULL;
    }

    c->enabled	     = 0;
    c->operator_lock = 0;
    c->aimon_enabled = 0;
    c->aimon_port    = MCP_AIMON_DEF_PORT;
    c->file_ok	     = 0;
}


void
mcp_free_list(char ***listp)
{
    char **p;

    if(listp == NULL || *listp == NULL)
      return;

    for(p = *listp; *p; p++)
      free(*p);

    free(*listp);
    *listp = NULL;
}


static char **
mcp_list_dup(char **list)
{
    char **d;
    int    n, i;

    for(n = 0; list && list[n]; n++)
      ;

    if((d = calloc((size_t) n + 1, sizeof(char *))) == NULL)
      return(NULL);

    for(i = 0; i < n; i++)
      if((d[i] = mcp_strdup(list[i])) == NULL){
	  mcp_free_list(&d);
	  return(NULL);
      }

    return(d);
}


/*
 * Host name: 1..255 characters, no ASCII control or whitespace (<= 0x20),
 * no DEL or high-byte characters (>= 0x7f).
 */
static int
valid_host(const char *h)
{
    const unsigned char *p;

    if(h == NULL || *h == '\0' || strlen(h) > 255)
      return(0);

    for(p = (const unsigned char *) h; *p; p++){
	if(*p <= ' ' || *p >= 0x7f)
	  return(0);
    }

    return(1);
}


/*
 * Domain: dot-separated labels of [a-z0-9-], no empty label, no label
 * that starts or ends with '-'.  Input is already lower case.
 */
static int
mcp_domain_valid(const char *d)
{
    const char *label = d;
    const char *p;
    size_t	len;

    if(d == NULL || *d == '\0')
      return(0);

    for(p = d; ; p++){
	if(*p == '.' || *p == '\0'){
	    len = (size_t)(p - label);
	    if(len == 0 || len > 63 || label[0] == '-' || p[-1] == '-')
	      return(0);

	    if(*p == '\0')
	      break;

	    label = p + 1;
	}
	else if(!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9')
		  || *p == '-'))
	  return(0);
    }

    return(1);
}


/*
 * Local part: RFC 5322 dot-atom characters, no leading, trailing or
 * doubled dot.  '*' is not legal here; "*@domain" is handled by the
 * caller.
 */
static int
mcp_local_valid(const char *l, size_t len)
{
    static const char *atext_extra = "!#$%&'+-/=?^_`{|}~";
    size_t i;

    if(len == 0 || len > 64 || l[0] == '.' || l[len-1] == '.')
      return(0);

    for(i = 0; i < len; i++){
	unsigned char ch = (unsigned char) l[i];

	if(ch == '.'){
	    if(l[i+1] == '.')
	      return(0);
	}
	else if(!(ch < 0x80
		  && (isalnum(ch) || (ch != '\0' && strchr(atext_extra, ch)))))
	  return(0);
    }

    return(1);
}


/*
 * Trim, lower-case and check one allow-list entry.  The only legal
 * forms are "local@domain" and "*@domain".  Returns a malloc'd string,
 * or NULL if the entry is not legal.
 */
static char *
mcp_allow_normalize(const char *in)
{
    const char *b, *e;
    char       *s, *at, *p;
    size_t	len;

    if(in == NULL)
      return(NULL);

    for(b = in; *b && isspace((unsigned char) *b); b++)
      ;

    for(e = b + strlen(b); e > b && isspace((unsigned char) e[-1]); e--)
      ;

    len = (size_t)(e - b);
    if(len == 0 || len > MCP_ADDR_MAX)
      return(NULL);

    if((s = malloc(len + 1)) == NULL)
      return(NULL);

    for(p = s; b < e; b++, p++)
      *p = (char) tolower((unsigned char) *b);

    *p = '\0';

    if((at = strchr(s, '@')) == NULL || strchr(at + 1, '@') != NULL
       || !mcp_domain_valid(at + 1)
       || !((at - s == 1 && s[0] == '*')
	    || mcp_local_valid(s, (size_t)(at - s)))){
	free(s);
	return(NULL);
    }

    return(s);
}


int
mcp_allow_entry_valid(char *entry)
{
    char *n;

    if((n = mcp_allow_normalize(entry)) == NULL)
      return(0);

    free(n);
    return(1);
}


/*
 * Fill c from root.  Returns 0 if root follows the schema, -1 with
 * c->last_error set if it does not.  Unknown keys are ignored.
 */
static int
mcp_conf_parse(MCP_CONFIG_S *c, cJSON *root)
{
    cJSON *mcp, *item, *aimon, *entry;
    char **list = NULL;
    int	   n = 0, bad = 0;

    if(!cJSON_IsObject(root)){
	mcp_conf_set_error(c, "top level is not an object");
	return(-1);
    }

    if((mcp = cJSON_GetObjectItemCaseSensitive(root, "mcp")) != NULL){
	if(!cJSON_IsObject(mcp)){
	    mcp_conf_set_error(c, "\"mcp\" is not an object");
	    return(-1);
	}

	if((item = cJSON_GetObjectItemCaseSensitive(mcp, "enabled")) != NULL){
	    if(!cJSON_IsBool(item)){
		mcp_conf_set_error(c, "\"mcp.enabled\" is not a boolean");
		return(-1);
	    }

	    c->enabled = cJSON_IsTrue(item) ? 1 : 0;
	}
    }

    if((item = cJSON_GetObjectItemCaseSensitive(root, "operator_lock")) != NULL){
	if(!cJSON_IsBool(item)){
	    mcp_conf_set_error(c, "\"operator_lock\" is not a boolean");
	    return(-1);
	}

	c->operator_lock = cJSON_IsTrue(item) ? 1 : 0;
    }

    if((item = cJSON_GetObjectItemCaseSensitive(root, "allowed_recipients")) != NULL){
	if(!cJSON_IsArray(item)){
	    mcp_conf_set_error(c, "\"allowed_recipients\" is not an array");
	    return(-1);
	}

	if((list = calloc(MCP_ALLOW_MAX + 1, sizeof(char *))) == NULL){
	    mcp_conf_set_error(c, "out of memory");
	    return(-1);
	}

	cJSON_ArrayForEach(entry, item){
	    char *norm;

	    if(!cJSON_IsString(entry)
	       || (norm = mcp_allow_normalize(entry->valuestring)) == NULL){
		bad++;			/* skipped: it allows nothing */
		continue;
	    }

	    if(n >= MCP_ALLOW_MAX){
		free(norm);
		bad++;
		continue;
	    }

	    list[n++] = norm;
	}

	c->allowed = list;
	if(bad)
	  mcp_conf_set_error(c, "%d allowed_recipients entr%s ignored",
			     bad, bad == 1 ? "y" : "ies");
    }

    if((aimon = cJSON_GetObjectItemCaseSensitive(root, "aimon")) != NULL){
	if(!cJSON_IsObject(aimon)){
	    mcp_conf_set_error(c, "\"aimon\" is not an object");
	    return(-1);
	}

	if((item = cJSON_GetObjectItemCaseSensitive(aimon, "enabled")) != NULL){
	    if(!cJSON_IsBool(item)){
		mcp_conf_set_error(c, "\"aimon.enabled\" is not a boolean");
		return(-1);
	    }

	    c->aimon_enabled = cJSON_IsTrue(item) ? 1 : 0;
	}

	if((item = cJSON_GetObjectItemCaseSensitive(aimon, "host")) != NULL){
	    const char *h;

	    if(!cJSON_IsString(item) || (h = item->valuestring) == NULL
	       || !valid_host(h)){
		mcp_conf_set_error(c, "\"aimon.host\" is not a host name");
		return(-1);
	    }

	    if((c->aimon_host = mcp_strdup(h)) == NULL){
		mcp_conf_set_error(c, "out of memory");
		return(-1);
	    }
	}

	if((item = cJSON_GetObjectItemCaseSensitive(aimon, "port")) != NULL){
	    double d;

	    if(!cJSON_IsNumber(item) || (d = item->valuedouble) < 1.0
	       || d > 65535.0 || d != (double)(int) d){
		mcp_conf_set_error(c, "\"aimon.port\" is not a port number");
		return(-1);
	    }

	    c->aimon_port = (int) d;
	}
    }

    return(0);
}


/*
 * Read the policy file once, on the main thread, before any MCP thread
 * starts.  homedir is $HOME.  Any problem leaves the defaults in place,
 * which is upstream Alpine.
 */
void
mcp_config_load(char *homedir)
{
    MCP_CONFIG_S c;
    struct stat  sb;
    char	*buf = NULL;
    ssize_t	 got;
    size_t	 have = 0;
    cJSON	*root = NULL;
    int		 fd = -1;

    memset(&c, 0, sizeof(c));
    mcp_conf_clear(&c);

    if(homedir == NULL || *homedir == '\0'){
	mcp_conf_set_error(&c, "no home directory");
	goto done;
    }

    if(snprintf(mcp_conf_dir, sizeof(mcp_conf_dir), "%s/%s",
		homedir, MCP_CONF_DIR) >= (int) sizeof(mcp_conf_dir)
       || snprintf(mcp_conf_path, sizeof(mcp_conf_path), "%s/%s",
		   mcp_conf_dir, MCP_CONF_FILE) >= (int) sizeof(mcp_conf_path)){
	mcp_conf_dir[0] = mcp_conf_path[0] = '\0';
	mcp_conf_set_error(&c, "config path too long");
	goto done;
    }

    if((fd = open(mcp_conf_path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC)) < 0){
	if(errno != ENOENT)
	  mcp_conf_set_error(&c, "open: %s", strerror(errno));

	goto done;			/* missing file: plain defaults */
    }

    if(fstat(fd, &sb) < 0){
	mcp_conf_set_error(&c, "fstat: %s", strerror(errno));
	goto done;
    }

    if(!S_ISREG(sb.st_mode)){
	mcp_conf_set_error(&c, "not a regular file");
	goto done;
    }

    if(sb.st_uid != getuid()){
	mcp_conf_set_error(&c, "not owned by this user");
	goto done;
    }

    if(sb.st_mode & 077){
	mcp_conf_set_error(&c, "mode is %03o, must be 600",
			   (unsigned)(sb.st_mode & 0777));
	goto done;
    }

    if(sb.st_size <= 0 || sb.st_size > MCP_CONF_MAX_BYTES){
	mcp_conf_set_error(&c, "size %ld out of range", (long) sb.st_size);
	goto done;
    }

    if((buf = malloc((size_t) sb.st_size + 1)) == NULL){
	mcp_conf_set_error(&c, "out of memory");
	goto done;
    }

    while(have < (size_t) sb.st_size){
	got = read(fd, buf + have, (size_t) sb.st_size - have);
	if(got < 0 && errno == EINTR)
	  continue;

	if(got <= 0)
	  break;

	have += (size_t) got;
    }

    if(have != (size_t) sb.st_size){
	mcp_conf_set_error(&c, "short read");
	goto done;
    }

    buf[have] = '\0';
    if((root = mcp_json_parse(buf, have)) == NULL){
	mcp_conf_set_error(&c, "invalid JSON");
	goto done;
    }

    if(mcp_conf_parse(&c, root) < 0){
	char keep[sizeof(c.last_error)];

	memcpy(keep, c.last_error, sizeof(keep));
	mcp_conf_clear(&c);		/* fail closed */
	memcpy(c.last_error, keep, sizeof(keep));
	goto done;
    }

    c.file_ok = 1;

#ifndef HAVE_PTHREAD
    /* the listener needs threads; without them MCP stays off */
    if(c.enabled){
	c.enabled = 0;
	mcp_conf_set_error(&c, "built without pthread support");
    }
#endif

  done:
    if(root)
      cJSON_Delete(root);

    if(buf)
      free(buf);

    if(fd >= 0)
      close(fd);

    pthread_mutex_lock(&mcp_conf_mutex);
    mcp_conf_clear(&mcp_conf);
    mcp_conf = c;
    pthread_mutex_unlock(&mcp_conf_mutex);
}


static int
mcp_write_all(int fd, const char *p, size_t len)
{
    ssize_t n;

    while(len > 0){
	n = write(fd, p, len);
	if(n < 0 && errno == EINTR)
	  continue;

	if(n <= 0)
	  return(-1);

	p   += n;
	len -= (size_t) n;
    }

    return(0);
}


/*
 * Write the current policy back to config.json: a temp file in the same
 * directory, mode 0600, fsync, rename, then fsync the directory.
 * Returns 0 on success, -1 with the error kept for the monitor screen.
 */
int
mcp_config_save(void)
{
    cJSON *root = NULL, *mcp, *aimon, *arr;
    char  *text = NULL;
    char   tmp[PATH_MAX];
    char **p;
    int	   fd = -1, dfd = -1, rv = -1;
    struct stat sb;

    pthread_mutex_lock(&mcp_conf_mutex);

    if(mcp_conf_path[0] == '\0'){
	mcp_conf_set_error(&mcp_conf, "no config path");
	goto out;
    }

    if((root = cJSON_CreateObject()) == NULL
       || (mcp = cJSON_AddObjectToObject(root, "mcp")) == NULL
       || cJSON_AddBoolToObject(mcp, "enabled", mcp_conf.enabled) == NULL
       || cJSON_AddBoolToObject(root, "operator_lock",
				mcp_conf.operator_lock) == NULL
       || (arr = cJSON_AddArrayToObject(root, "allowed_recipients")) == NULL
       || (aimon = cJSON_AddObjectToObject(root, "aimon")) == NULL
       || cJSON_AddBoolToObject(aimon, "enabled", mcp_conf.aimon_enabled) == NULL
       || cJSON_AddStringToObject(aimon, "host", mcp_conf.aimon_host
				  ? mcp_conf.aimon_host
				  : MCP_AIMON_DEF_HOST) == NULL
       || cJSON_AddNumberToObject(aimon, "port", mcp_conf.aimon_port) == NULL){
	mcp_conf_set_error(&mcp_conf, "out of memory");
	goto out;
    }

    for(p = mcp_conf.allowed; p && *p; p++){
	cJSON *s;

	if((s = cJSON_CreateString(*p)) == NULL
	   || !cJSON_AddItemToArray(arr, s)){
	    if(s)
	      cJSON_Delete(s);

	    mcp_conf_set_error(&mcp_conf, "out of memory");
	    goto out;
	}
    }

    if((text = cJSON_Print(root)) == NULL){
	mcp_conf_set_error(&mcp_conf, "out of memory");
	goto out;
    }

    /* the parent (~/.config) may not exist on a fresh account */
    {
	char *slash;

	snprintf(tmp, sizeof(tmp), "%s", mcp_conf_dir);
	if((slash = strrchr(tmp, '/')) != NULL && slash != tmp){
	    *slash = '\0';
	    if(mkdir(tmp, 0700) < 0 && errno != EEXIST){
		mcp_conf_set_error(&mcp_conf, "mkdir: %s", strerror(errno));
		goto out;
	    }
	}
    }

    if(mkdir(mcp_conf_dir, 0700) < 0 && errno != EEXIST){
	mcp_conf_set_error(&mcp_conf, "mkdir: %s", strerror(errno));
	goto out;
    }

    if(lstat(mcp_conf_dir, &sb) < 0 || !S_ISDIR(sb.st_mode)
       || sb.st_uid != getuid()){
	mcp_conf_set_error(&mcp_conf, "config directory is not ours");
	goto out;
    }

    if(snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", mcp_conf_path,
		(long) getpid()) >= (int) sizeof(tmp)){
	mcp_conf_set_error(&mcp_conf, "config path too long");
	goto out;
    }

    (void) unlink(tmp);
    if((fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
		  0600)) < 0){
	mcp_conf_set_error(&mcp_conf, "create: %s", strerror(errno));
	goto out;
    }

    if(fchmod(fd, 0600) < 0
       || mcp_write_all(fd, text, strlen(text)) < 0
       || mcp_write_all(fd, "\n", 1) < 0
       || fsync(fd) < 0){
	mcp_conf_set_error(&mcp_conf, "write: %s", strerror(errno));
	(void) close(fd);
	fd = -1;
	(void) unlink(tmp);
	goto out;
    }

    if(close(fd) < 0){
	fd = -1;
	mcp_conf_set_error(&mcp_conf, "close: %s", strerror(errno));
	(void) unlink(tmp);
	goto out;
    }

    fd = -1;
    if(rename(tmp, mcp_conf_path) < 0){
	mcp_conf_set_error(&mcp_conf, "rename: %s", strerror(errno));
	(void) unlink(tmp);
	goto out;
    }

    if((dfd = open(mcp_conf_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC)) >= 0){
	(void) fsync(dfd);
	(void) close(dfd);
    }

    mcp_conf.file_ok = 1;
    mcp_conf.last_error[0] = '\0';
    rv = 0;

  out:
    pthread_mutex_unlock(&mcp_conf_mutex);

    if(text)
      cJSON_free(text);

    if(root)
      cJSON_Delete(root);

    return(rv);
}


/* fixed at startup, so no lock is needed to read it */
int
mcp_enabled(void)
{
    return(mcp_conf.enabled);
}


int
mcp_lock_engaged(void)
{
    int v;

    pthread_mutex_lock(&mcp_conf_mutex);
    v = mcp_conf.operator_lock;
    pthread_mutex_unlock(&mcp_conf_mutex);
    return(v);
}


void
mcp_set_lock(int on)
{
    pthread_mutex_lock(&mcp_conf_mutex);
    mcp_conf.operator_lock = on ? 1 : 0;
    pthread_mutex_unlock(&mcp_conf_mutex);
}


int
mcp_aimon_enabled(void)
{
    int v;

    pthread_mutex_lock(&mcp_conf_mutex);
    v = mcp_conf.enabled && mcp_conf.aimon_enabled;
    pthread_mutex_unlock(&mcp_conf_mutex);
    return(v);
}


void
mcp_set_aimon_enabled(int on)
{
    pthread_mutex_lock(&mcp_conf_mutex);
    mcp_conf.aimon_enabled = on ? 1 : 0;
    pthread_mutex_unlock(&mcp_conf_mutex);
}


/*
 * Copy the aimon host into host (hostlen bytes) and the port into
 * *port.  Returns 0, or -1 if host does not fit.
 */
int
mcp_aimon_target(char *host, size_t hostlen, int *port)
{
    const char *h;
    int		rv = 0;

    pthread_mutex_lock(&mcp_conf_mutex);
    h = mcp_conf.aimon_host ? mcp_conf.aimon_host : MCP_AIMON_DEF_HOST;
    if(host == NULL || hostlen == 0
       || snprintf(host, hostlen, "%s", h) >= (int) hostlen)
      rv = -1;

    if(port)
      *port = mcp_conf.aimon_port;

    pthread_mutex_unlock(&mcp_conf_mutex);
    return(rv);
}


/*
 * Set the aimon gateway host and port in memory. Validates that host
 * is non-empty, <= 255 characters, and free of whitespace/control characters,
 * and that port is between 1 and 65535. Returns 0 on success, or -1 with
 * err populated on failure. Previous target is preserved on error.
 */
int
mcp_set_aimon_target(const char *host, int port, char *err, size_t errlen)
{
    char *copy;

    if(!valid_host(host)){
	if(err && errlen > 0)
	  snprintf(err, errlen, "invalid host: %.60s", host ? host : "(null)");

	return(-1);
    }

    if(port < 1 || port > 65535){
	if(err && errlen > 0)
	  snprintf(err, errlen, "port %d out of range (1-65535)", port);

	return(-1);
    }

    if((copy = mcp_strdup(host)) == NULL){
	if(err && errlen > 0)
	  snprintf(err, errlen, "out of memory");

	return(-1);
    }

    pthread_mutex_lock(&mcp_conf_mutex);
    if(mcp_conf.aimon_host)
      free(mcp_conf.aimon_host);

    mcp_conf.aimon_host = copy;
    mcp_conf.aimon_port = port;
    pthread_mutex_unlock(&mcp_conf_mutex);

    if(err && errlen > 0)
      err[0] = '\0';

    return(0);
}


int
mcp_allow_count(void)
{
    int n = 0;

    pthread_mutex_lock(&mcp_conf_mutex);
    while(mcp_conf.allowed && mcp_conf.allowed[n])
      n++;

    pthread_mutex_unlock(&mcp_conf_mutex);
    return(n);
}


/*
 * Is addr (a bare "local@domain") on the allow list?  An exact entry
 * matches that address.  A "*@domain" entry matches every address at
 * exactly that domain, not its subdomains.  An empty list matches
 * nothing.
 */
int
mcp_recipient_allowed(char *addr)
{
    char  *norm, *at;
    char **p;
    int	   hit = 0;

    if((norm = mcp_allow_normalize(addr)) == NULL)
      return(0);

    if(norm[0] == '*'){			/* a wildcard is not an address */
	free(norm);
	return(0);
    }

    at = strchr(norm, '@');

    pthread_mutex_lock(&mcp_conf_mutex);
    for(p = mcp_conf.allowed; !hit && p && *p; p++){
	if((*p)[0] == '*' && (*p)[1] == '@')
	  hit = !strcmp(*p + 2, at + 1);
	else
	  hit = !strcmp(*p, norm);
    }

    pthread_mutex_unlock(&mcp_conf_mutex);

    free(norm);
    return(hit);
}


/* malloc'd, NULL-terminated copy; free with mcp_free_list */
char **
mcp_allow_list_copy(void)
{
    char **d;

    pthread_mutex_lock(&mcp_conf_mutex);
    d = mcp_list_dup(mcp_conf.allowed);
    pthread_mutex_unlock(&mcp_conf_mutex);
    return(d);
}


/*
 * Replace the allow list with list (NULL terminated).  Every entry must
 * be legal.  Duplicates are dropped.  On a bad entry nothing changes,
 * the entry is named in err, and -1 is returned.
 */
int
mcp_set_allow_list(char **list, char *err, size_t errlen)
{
    char **norm;
    int	   n, i, j, k;

    for(n = 0; list && list[n]; n++)
      ;

    if(n > MCP_ALLOW_MAX){
	if(err && errlen)
	  snprintf(err, errlen, "more than %d entries", MCP_ALLOW_MAX);

	return(-1);
    }

    if((norm = calloc((size_t) n + 1, sizeof(char *))) == NULL){
	if(err && errlen)
	  snprintf(err, errlen, "out of memory");

	return(-1);
    }

    for(i = 0, k = 0; i < n; i++){
	char *s;

	if((s = mcp_allow_normalize(list[i])) == NULL){
	    if(err && errlen)
	      snprintf(err, errlen, "not local@domain or *@domain: %.60s",
		       list[i]);

	    mcp_free_list(&norm);
	    return(-1);
	}

	for(j = 0; j < k; j++)
	  if(!strcmp(norm[j], s))
	    break;

	if(j < k)
	  free(s);
	else
	  norm[k++] = s;
    }

    pthread_mutex_lock(&mcp_conf_mutex);
    mcp_free_list(&mcp_conf.allowed);
    mcp_conf.allowed = norm;
    pthread_mutex_unlock(&mcp_conf_mutex);

    if(err && errlen)
      err[0] = '\0';

    return(0);
}


char *
mcp_config_path(void)
{
    return(mcp_conf_path);
}


void
mcp_config_last_error(char *buf, size_t len)
{
    if(buf == NULL || len == 0)
      return;

    pthread_mutex_lock(&mcp_conf_mutex);
    snprintf(buf, len, "%s", mcp_conf.last_error);
    pthread_mutex_unlock(&mcp_conf_mutex);
}


int
mcp_config_ok(void)
{
    int ok;

    pthread_mutex_lock(&mcp_conf_mutex);
    ok = mcp_conf.file_ok && (mcp_conf.last_error[0] == '\0');
    pthread_mutex_unlock(&mcp_conf_mutex);
    return(ok);
}


int
mcp_config_error(void)
{
    int err;

    pthread_mutex_lock(&mcp_conf_mutex);
    err = (mcp_conf.last_error[0] != '\0');
    pthread_mutex_unlock(&mcp_conf_mutex);
    return(err);
}


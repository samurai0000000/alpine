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
 * MCP listener, queue and main-thread drain.
 *
 * Threads:
 *   listener  accepts Unix-socket clients, splits NDJSON lines, answers
 *             initialize / ping / tools/list itself, checks the operator
 *             lock and the argument schema for tools/call, and enqueues.
 *             It never calls pith or c-client.
 *   main      Alpine's own thread.  mcp_drain() runs queued tools and
 *             hands finished responses back to the thread that owns the
 *             client connection.
 *
 * The listener wakes the main thread through a self-pipe (wake_pipe).
 * The main thread wakes the listener through a second pipe (lwake_pipe)
 * when a response is ready.  All socket I/O stays on the listener.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1		/* accept4, pipe2, struct ucred */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "mcpjson.h"
#include "mcpconf.h"
#include "mcp.h"
#include "mcpint.h"

#ifndef PACKAGE_VERSION
#define PACKAGE_VERSION "2.26"
#endif

#define MCP_MAX_CONN	16			/* socket clients at once */
#define MCP_MAX_LINE	(4 * 1024 * 1024)	/* one request line */
#define MCP_MAX_WBUF	(64 * 1024 * 1024)	/* unread output per client */
#define MCP_OWNER_SOCKET 1
#define MCP_OWNER_AIMON  2
#define MCP_AIMON_TIMEOUT_MS 5000		/* connect, and register reply */


typedef struct mcp_item {
    unsigned long    conn_id;
    int		     owner;
    cJSON	    *id;		/* request id, number or string */
    MCP_TOOL_S	    *tool;
    cJSON	    *args;
    int		     cancelled;		/* client went away */
    char	    *response;		/* finished line, no newline */
    char	    *prompt;		/* parked: the question for the human */
    time_t	     parked_at;
    struct mcp_item *next;
} MCP_ITEM_S;


typedef struct mcp_conn {
    int		  fd;
    unsigned long id;
    char	 *rbuf;
    size_t	  rlen, rcap;
    char	 *wbuf;
    size_t	  wlen, wcap;
    int		  read_closed;		/* peer shut down its write side */
    int		  outstanding;		/* tools/call not yet answered */
    int		  owner;		/* MCP_OWNER_SOCKET or MCP_OWNER_AIMON */
    int		  reg_pending;		/* aimon: gateway/register unanswered */
} MCP_CONN_S;


/*
 * Queue state, guarded by q_mutex
 */
static pthread_mutex_t q_mutex = PTHREAD_MUTEX_INITIALIZER;
static MCP_ITEM_S     *q_head, *q_tail;		/* waiting to run */
static int	       q_len;
static MCP_ITEM_S     *done_head, *done_tail;	/* finished, not yet sent */
static MCP_ITEM_S     *park_head, *park_tail;	/* waiting for the human */
static int	       park_len;
static int	       park_new;		/* a call was parked since last asked */
static MCP_ITEM_S     *asking;		/* parked call whose question is on screen */
static int	       stop_listener;
static char	       srv_error[256];

/*
 * Set once on the main thread before the listener starts
 */
static int	       srv_running;
static int	       wake_pipe[2]  = {-1, -1};	/* listener -> main */
static int	       lwake_pipe[2] = {-1, -1};	/* main -> listener */
static int	       listen_fd = -1;
static char	       sock_path[sizeof(((struct sockaddr_un *) 0)->sun_path)];
static dev_t	       sock_dev;
static ino_t	       sock_ino;
static pthread_t       listener_tid;

/*
 * Listener-thread only
 */
static MCP_CONN_S      conns[MCP_MAX_CONN];
static int	       nconns;
static unsigned long   next_conn_id = 1;

/*
 * Listener-thread only: aimon gateway client, plan section 2.6
 */
#define AIMON_OFF		0
#define AIMON_WAIT		1	/* backing off, retry at aimon_deadline */
#define AIMON_CONNECTING	2	/* non-blocking connect in flight */
#define AIMON_REGISTERING	3	/* connected, register reply awaited */
#define AIMON_UP		4

static const int       aimon_backoff_s[] = { 1, 2, 4, 8, 16, 30 };
static int	       aimon_state;
static int	       aimon_fd = -1;		/* only while CONNECTING */
static long long       aimon_deadline;		/* monotonic milliseconds */
static int	       aimon_backoff;		/* index into aimon_backoff_s */
static int	       aimon_closing;		/* teardown in progress */
static char	       aimon_why[96];		/* reason for the next close */

/*
 * Guarded by q_mutex: written by the listener, read by the monitor
 * screen; and the request flag the main thread sets.
 */
static char	       aimon_status[200] = "off";
static int	       aimon_drop_req;

/*
 * Main-thread only: state of the tool now running
 */
static int	       in_tool;
static int	       cur_login_needed;
static char	       cur_login_host[256];
static char	       cur_cclient_error[512];
static int	       in_drain;
static int	       in_confirm;
#define MCP_PARK_AIMON_SECS 40	/* aimon gives up on a call after 45 */



/*
 * Internal prototypes
 */
static void	    srv_set_error(const char *, ...);
static void	   *mcp_listener_main(void *);
static void	    mcp_item_free(MCP_ITEM_S *);
static void	    mcp_cancel_conn(unsigned long);
static void	    mcp_complete(MCP_ITEM_S *);
static MCP_ITEM_S  *mcp_dequeue(void);
static int	    mcp_enqueue(MCP_ITEM_S *);
static void	    conn_close(int);
static int	    conn_send(MCP_CONN_S *, const char *);
static int	    conn_flush(MCP_CONN_S *);
static void	    conn_read(MCP_CONN_S *);
static void	    conn_line(MCP_CONN_S *, char *, size_t);
static void	    deliver_done(void);
static char	   *handle_request(cJSON *, unsigned long, int, int *);
static int	    set_nonblock(int);
static void	    pipe_poke(int);
static void	    pipe_drain(int);
static void	    aimon_conn_gone(void);
static int	    aimon_reply(MCP_CONN_S *, cJSON *);


static void
srv_set_error(const char *fmt, ...)
{
    va_list ap;

    pthread_mutex_lock(&q_mutex);
    va_start(ap, fmt);
    vsnprintf(srv_error, sizeof(srv_error), fmt, ap);
    va_end(ap);
    pthread_mutex_unlock(&q_mutex);
}


void
mcp_server_last_error(char *buf, size_t len)
{
    if(buf == NULL || len == 0)
      return;

    pthread_mutex_lock(&q_mutex);
    snprintf(buf, len, "%s", srv_error);
    pthread_mutex_unlock(&q_mutex);
}


char *
mcp_socket_path(char *buf, size_t len)
{
    if(buf && len)
      snprintf(buf, len, "%s", sock_path);

    return(buf);
}


static int
set_nonblock(int fd)
{
    int fl;

    if((fl = fcntl(fd, F_GETFL)) < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0)
      return(-1);

    return(0);
}


/* one byte; a full pipe already means "wake up" */
static void
pipe_poke(int fd)
{
    char    c = 1;
    ssize_t n;

    if(fd < 0)
      return;

    do
      n = write(fd, &c, 1);
    while(n < 0 && errno == EINTR);
}


static void
pipe_drain(int fd)
{
    char    buf[256];
    ssize_t n;

    if(fd < 0)
      return;

    do
      n = read(fd, buf, sizeof(buf));
    while(n > 0 || (n < 0 && errno == EINTR));
}


/*----------------------------------------------------------------------
    JSON-RPC line builders.  Any thread.  The caller frees the result.
  ----*/
static char *
mcp_print_envelope(cJSON *id, cJSON *result, cJSON *error)
{
    cJSON *msg;
    char  *s = NULL;

    if((msg = cJSON_CreateObject()) == NULL){
	if(result)
	  cJSON_Delete(result);

	if(error)
	  cJSON_Delete(error);

	return(NULL);
    }

    cJSON_AddStringToObject(msg, "jsonrpc", "2.0");
    if(id)
      cJSON_AddItemToObject(msg, "id", cJSON_Duplicate(id, 1));
    else
      cJSON_AddNullToObject(msg, "id");

    if(error)
      cJSON_AddItemToObject(msg, "error", error);
    else
      cJSON_AddItemToObject(msg, "result", result ? result : cJSON_CreateObject());

    s = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    return(s);
}


/* result is consumed */
char *
mcp_make_result(cJSON *id, cJSON *result)
{
    return(mcp_print_envelope(id, result, NULL));
}


char *
mcp_make_error(cJSON *id, int code, const char *message)
{
    cJSON *err;

    if((err = cJSON_CreateObject()) == NULL)
      return(NULL);

    cJSON_AddNumberToObject(err, "code", code);
    cJSON_AddStringToObject(err, "message", message ? message : "error");
    return(mcp_print_envelope(id, NULL, err));
}


void
mcp_call_error(MCP_CALL_S *call, int code, const char *fmt, ...)
{
    va_list ap;

    call->code = code;
    va_start(ap, fmt);
    vsnprintf(call->message, sizeof(call->message), fmt, ap);
    va_end(ap);
    if(call->result){
	cJSON_Delete(call->result);
	call->result = NULL;
    }
}


/*----------------------------------------------------------------------
    Per-call state.  Main thread.  c-client callbacks write it through
    mcp_note_*; the tool that is running reads it.
  ----*/
int
mcp_noninteractive(void)
{
    return(in_tool);
}


void
mcp_note_login_needed(char *host)
{
    if(!in_tool)
      return;

    cur_login_needed = 1;
    snprintf(cur_login_host, sizeof(cur_login_host), "%s", host ? host : "");
}


void
mcp_note_cclient_msg(char *msg, long errflg)
{
    (void) errflg;
    if(!in_tool || msg == NULL)
      return;

    snprintf(cur_cclient_error, sizeof(cur_cclient_error), "%s", msg);
}


int
mcp_cur_login_needed(char *host, size_t len)
{
    if(cur_login_needed && host && len)
      snprintf(host, len, "%s", cur_login_host);

    return(cur_login_needed);
}


int
mcp_cur_cclient_error(char *buf, size_t len)
{
    if(cur_cclient_error[0] && buf && len)
      snprintf(buf, len, "%s", cur_cclient_error);

    return(cur_cclient_error[0] != '\0');
}


void
mcp_cur_reset(void)
{
    cur_login_needed = 0;
    cur_login_host[0] = '\0';
    cur_cclient_error[0] = '\0';
}


/*----------------------------------------------------------------------
    Queue.  q_mutex guards everything below.
  ----*/
static void
mcp_item_free(MCP_ITEM_S *it)
{
    if(it == NULL)
      return;

    if(it->id)
      cJSON_Delete(it->id);

    if(it->args)
      cJSON_Delete(it->args);

    if(it->response)
      cJSON_free(it->response);

    if(it->prompt)
      free(it->prompt);

    free(it);
}


/* 0 on success, -1 if the queue already holds MCP_QUEUE_MAX items */
static int
mcp_enqueue(MCP_ITEM_S *it)
{
    int rv = -1;

    pthread_mutex_lock(&q_mutex);
    if(q_len < MCP_QUEUE_MAX){
	it->next = NULL;
	if(q_tail)
	  q_tail->next = it;
	else
	  q_head = it;

	q_tail = it;
	q_len++;
	rv = 0;
    }

    pthread_mutex_unlock(&q_mutex);

    if(rv == 0)
      pipe_poke(wake_pipe[1]);

    return(rv);
}


static MCP_ITEM_S *
mcp_dequeue(void)
{
    MCP_ITEM_S *it;

    pthread_mutex_lock(&q_mutex);
    if((it = q_head) != NULL){
	if((q_head = it->next) == NULL)
	  q_tail = NULL;

	q_len--;
	it->next = NULL;
    }

    pthread_mutex_unlock(&q_mutex);
    return(it);
}


/* the client is gone: nothing it queued may start */
static void
mcp_cancel_conn(unsigned long conn_id)
{
    MCP_ITEM_S *it;

    pthread_mutex_lock(&q_mutex);
    for(it = q_head; it; it = it->next)
      if(it->conn_id == conn_id)
	it->cancelled = 1;

    for(it = park_head; it; it = it->next)
      if(it->conn_id == conn_id)
	it->cancelled = 1;

    if(asking && asking->conn_id == conn_id)
      asking->cancelled = 1;

    pthread_mutex_unlock(&q_mutex);
}


/* main thread: hand a finished item to the thread that owns its client */
static void
mcp_complete(MCP_ITEM_S *it)
{
    pthread_mutex_lock(&q_mutex);
    it->next = NULL;
    if(done_tail)
      done_tail->next = it;
    else
      done_head = it;

    done_tail = it;
    pthread_mutex_unlock(&q_mutex);

    pipe_poke(lwake_pipe[1]);
}


/*----------------------------------------------------------------------
    Main-thread drain.  Called when the wake pipe is readable, and from
    the command loop between keystrokes.  Never re-entered.
  ----*/
/*
 * Run one tool on the main thread and build its response.  fn is the
 * tool's run function, or its run_confirmed part after the human said
 * yes.  When park_ok is set and the tool asks to wait for the human, the
 * item goes on the parked list and nothing is answered yet; returns 1
 * then, else 0 with it->response set.
 */
static int
exec_item(MCP_ITEM_S *it, void (*fn)(cJSON *, MCP_CALL_S *), int park_ok)
{
    MCP_CALL_S call;

    memset(&call, 0, sizeof(call));

    mcp_cur_reset();
    in_tool = 1;
    (*fn)(it->args, &call);
    in_tool = 0;

    if(call.code == 0 && call.park && park_ok){
	/* wait for the human; answered later */
	it->prompt = strdup(call.park_prompt);
	it->parked_at = time(NULL);
	it->next = NULL;
	pthread_mutex_lock(&q_mutex);
	if(park_tail)
	  park_tail->next = it;
	else
	  park_head = it;

	park_tail = it;
	park_len++;
	park_new = 1;
	pthread_mutex_unlock(&q_mutex);
	if(call.result)
	  cJSON_Delete(call.result);

	return(1);
    }

    if(call.code == 0){
	cJSON *res, *content, *text;
	char  *body;

	body = call.result ? cJSON_PrintUnformatted(call.result) : NULL;
	res = cJSON_CreateObject();
	content = cJSON_AddArrayToObject(res, "content");
	text = cJSON_CreateObject();
	cJSON_AddStringToObject(text, "type", "text");
	cJSON_AddStringToObject(text, "text", body ? body : "{}");
	cJSON_AddItemToArray(content, text);
	cJSON_AddBoolToObject(res, "isError", 0);
	it->response = mcp_make_result(it->id, res);
	if(body)
	  cJSON_free(body);
    }
    else
      it->response = mcp_make_error(it->id, call.code, call.message);

    if(call.result)
      cJSON_Delete(call.result);

    return(0);
}


int
mcp_drain(void)
{
    MCP_ITEM_S *it;
    int		count = 0;

    if(!srv_running || in_drain)
      return(0);

    in_drain = 1;
    pipe_drain(wake_pipe[0]);

    while((it = mcp_dequeue()) != NULL){
	count++;
	if(it->cancelled){
	    it->response = mcp_make_error(it->id, MCP_ERR_CANCELLED,
					  "request cancelled before it started");
	}
	else if(mcp_lock_engaged()){
	    it->response = mcp_make_error(it->id, MCP_ERR_LOCKED,
					  "operator lock is engaged");
	}
	else if(exec_item(it, it->tool->run, it->tool->run_confirmed != NULL))
	  continue;				/* parked */

	mcp_complete(it);
    }

    in_drain = 0;
    return(count);
}


/*
 * Put the parked questions to the human.  Called from Alpine's command
 * loop, never from inside the drain, a login or a modal screen.  A yes
 * runs the tool's confirmed part; a no, a vanished client, a lock that
 * went on meanwhile, or (for aimon, which gives up on a call after 45
 * seconds) an answer that would come too late, ends the call with
 * -32007 or -32001 and nothing is sent.
 */
void
mcp_confirm_pending(int (*ask)(const char *))
{
    MCP_ITEM_S *it;
    int		yes;

    if(!srv_running || in_drain || in_confirm || ask == NULL)
      return;

    in_confirm = 1;
    for(;;){
	pthread_mutex_lock(&q_mutex);
	if((it = park_head) != NULL){
	    if((park_head = it->next) == NULL)
	      park_tail = NULL;

	    park_len--;
	    it->next = NULL;
	}

	pthread_mutex_unlock(&q_mutex);
	if(it == NULL)
	  break;

	if(it->cancelled
	   || (it->owner == MCP_OWNER_AIMON
	       && time(NULL) - it->parked_at > MCP_PARK_AIMON_SECS)){
	    it->response = mcp_make_error(it->id, MCP_ERR_DECLINED,
					  it->cancelled
					    ? "the client went away before the question was answered"
					    : "the question was not answered in time");
	    mcp_complete(it);
	    continue;
	}

	/* the question stays up until answered; a client that leaves
	   meanwhile must still cancel the call */
	pthread_mutex_lock(&q_mutex);
	asking = it;
	pthread_mutex_unlock(&q_mutex);

	yes = (*ask)(it->prompt ? it->prompt : "Allow this?");

	pthread_mutex_lock(&q_mutex);
	asking = NULL;
	pthread_mutex_unlock(&q_mutex);

	if(it->cancelled || mcp_lock_engaged()){
	    it->response = it->cancelled
		? mcp_make_error(it->id, MCP_ERR_DECLINED,
				 "the client went away before the question was answered")
		: mcp_make_error(it->id, MCP_ERR_LOCKED,
				 "operator lock is engaged");
	}
	else if(!yes){
	    it->response = mcp_make_error(it->id, MCP_ERR_DECLINED,
					  "the human answered no");
	}
	else
	  (void) exec_item(it, it->tool->run_confirmed, 0);

	mcp_complete(it);
    }

    in_confirm = 0;
}


int
mcp_take_new_park(void)
{
    int n;

    pthread_mutex_lock(&q_mutex);
    n = park_new;
    park_new = 0;
    pthread_mutex_unlock(&q_mutex);
    return(n);
}


int
mcp_pending_confirms(void)
{
    int n;

    pthread_mutex_lock(&q_mutex);
    n = park_len;
    pthread_mutex_unlock(&q_mutex);
    return(n);
}


/* -1 while a drain runs, so a nested input_ready cannot spin on it */
int
mcp_wake_fd(void)
{
    return((srv_running && !in_drain) ? wake_pipe[0] : -1);
}


int
mcp_server_running(void)
{
    return(srv_running);
}


/*----------------------------------------------------------------------
    Request handling, listener thread.  Returns a response line to send,
    or NULL when nothing is sent (notification, or the call was queued).
    *queued is set when a tools/call went onto the queue.
  ----*/
static char *
handle_request(cJSON *req, unsigned long conn_id, int owner, int *queued)
{
    cJSON      *jv, *method, *id, *params, *name, *args;
    MCP_TOOL_S *tool;
    MCP_ITEM_S *it;
    char	err[256];
    int		notify;

    *queued = 0;

    if(cJSON_IsArray(req))		/* batching is not part of 2025-11-25 */
      return(mcp_make_error(NULL, MCP_ERR_INVALID_REQ, "batch requests are not supported"));

    if(!cJSON_IsObject(req))
      return(mcp_make_error(NULL, MCP_ERR_INVALID_REQ, "request is not an object"));

    id	   = cJSON_GetObjectItemCaseSensitive(req, "id");
    method = cJSON_GetObjectItemCaseSensitive(req, "method");
    jv	   = cJSON_GetObjectItemCaseSensitive(req, "jsonrpc");
    notify = (id == NULL);

    if(id && !cJSON_IsString(id) && !cJSON_IsNumber(id))
      return(mcp_make_error(NULL, MCP_ERR_INVALID_REQ, "id must be a string or a number"));

    if(!cJSON_IsString(jv) || strcmp(jv->valuestring, "2.0")){
	return(notify ? NULL
		      : mcp_make_error(id, MCP_ERR_INVALID_REQ, "jsonrpc must be \"2.0\""));
    }

    if(method == NULL){
	/* a response from the peer; we never send requests on this hop */
	if(cJSON_GetObjectItemCaseSensitive(req, "result")
	   || cJSON_GetObjectItemCaseSensitive(req, "error"))
	  return(NULL);

	return(notify ? NULL
		      : mcp_make_error(id, MCP_ERR_INVALID_REQ, "method is missing"));
    }

    if(!cJSON_IsString(method))
      return(notify ? NULL
		    : mcp_make_error(id, MCP_ERR_INVALID_REQ, "method must be a string"));

    if(notify)			/* notifications/initialized and friends */
      return(NULL);

    if(!strcmp(method->valuestring, "initialize")){
	cJSON *res, *caps, *tools, *info;

	res = cJSON_CreateObject();
	cJSON_AddStringToObject(res, "protocolVersion", MCP_PROTOCOL_VERSION);
	caps = cJSON_AddObjectToObject(res, "capabilities");
	tools = cJSON_AddObjectToObject(caps, "tools");
	cJSON_AddBoolToObject(tools, "listChanged", 0);
	info = cJSON_AddObjectToObject(res, "serverInfo");
	cJSON_AddStringToObject(info, "name", MCP_SERVER_NAME);
	cJSON_AddStringToObject(info, "version", PACKAGE_VERSION);
	return(mcp_make_result(id, res));
    }

    if(!strcmp(method->valuestring, "ping"))
      return(mcp_make_result(id, cJSON_CreateObject()));

    if(!strcmp(method->valuestring, "tools/list"))
      return(mcp_make_result(id, mcp_tools_list_json()));

    if(strcmp(method->valuestring, "tools/call"))
      return(mcp_make_error(id, MCP_ERR_NO_METHOD, "method not found"));

    /* tools/call */
    params = cJSON_GetObjectItemCaseSensitive(req, "params");
    if(!cJSON_IsObject(params))
      return(mcp_make_error(id, MCP_ERR_PARAMS, "params must be an object"));

    name = cJSON_GetObjectItemCaseSensitive(params, "name");
    if(!cJSON_IsString(name))
      return(mcp_make_error(id, MCP_ERR_PARAMS, "params.name must be a string"));

    /* the lock comes first: every tools/call is refused, known or not */
    if(mcp_lock_engaged())
      return(mcp_make_error(id, MCP_ERR_LOCKED, "operator lock is engaged"));

    if((tool = mcp_tool_lookup(name->valuestring)) == NULL)
      return(mcp_make_error(id, MCP_ERR_NO_METHOD, "unknown tool"));

    args = cJSON_GetObjectItemCaseSensitive(params, "arguments");
    if(args != NULL && !cJSON_IsObject(args))
      return(mcp_make_error(id, MCP_ERR_PARAMS, "params.arguments must be an object"));

    err[0] = '\0';
    if((*tool->validate)(args, err, sizeof(err)) < 0)
      return(mcp_make_error(id, MCP_ERR_PARAMS, err[0] ? err : "invalid arguments"));

    if((it = calloc(1, sizeof(*it))) == NULL)
      return(mcp_make_error(id, MCP_ERR_INTERNAL, "out of memory"));

    it->conn_id = conn_id;
    it->owner	= owner;
    it->tool	= tool;
    it->id	= cJSON_Duplicate(id, 1);
    it->args	= args ? cJSON_Duplicate(args, 1) : cJSON_CreateObject();
    if(it->id == NULL || it->args == NULL){
	mcp_item_free(it);
	return(mcp_make_error(id, MCP_ERR_INTERNAL, "out of memory"));
    }

    if(mcp_enqueue(it) < 0){
	mcp_item_free(it);
	return(mcp_make_error(id, MCP_ERR_QUEUE_FULL, "queue is full"));
    }

    *queued = 1;
    return(NULL);
}


/*----------------------------------------------------------------------
    Connections, listener thread
  ----*/
static int
conn_send(MCP_CONN_S *c, const char *line)
{
    size_t len = strlen(line);

    if(c->wlen + len + 1 > MCP_MAX_WBUF)
      return(-1);			/* client stopped reading */

    if(c->wlen + len + 1 > c->wcap){
	size_t ncap = c->wcap ? c->wcap : 4096;
	char  *nb;

	while(ncap < c->wlen + len + 1)
	  ncap *= 2;

	if((nb = realloc(c->wbuf, ncap)) == NULL)
	  return(-1);

	c->wbuf = nb;
	c->wcap = ncap;
    }

    memcpy(c->wbuf + c->wlen, line, len);
    c->wlen += len;
    c->wbuf[c->wlen++] = '\n';
    return(0);
}


/* -1 if the peer is gone */
static int
conn_flush(MCP_CONN_S *c)
{
    ssize_t n;

    while(c->wlen > 0){
	n = send(c->fd, c->wbuf, c->wlen, MSG_NOSIGNAL);
	if(n < 0){
	    if(errno == EINTR)
	      continue;

	    if(errno == EAGAIN || errno == EWOULDBLOCK)
	      return(0);

	    return(-1);
	}

	memmove(c->wbuf, c->wbuf + n, c->wlen - (size_t) n);
	c->wlen -= (size_t) n;
    }

    return(0);
}


static void
conn_close(int i)
{
    MCP_CONN_S *c = &conns[i];
    int		was_aimon = (c->owner == MCP_OWNER_AIMON);

    mcp_cancel_conn(c->id);
    if(c->fd >= 0)
      close(c->fd);

    free(c->rbuf);
    free(c->wbuf);

    if(i != nconns - 1)
      conns[i] = conns[nconns - 1];

    memset(&conns[nconns - 1], 0, sizeof(MCP_CONN_S));
    conns[nconns - 1].fd = -1;
    nconns--;

    if(was_aimon)
      aimon_conn_gone();
}


static void
conn_line(MCP_CONN_S *c, char *line, size_t len)
{
    cJSON *req;
    char  *resp;
    int	   queued = 0;

    if(len > 0 && line[len - 1] == '\r')
      line[--len] = '\0';

    if(len == 0)
      return;

    if((req = mcp_json_parse(line, len)) == NULL)
      resp = mcp_make_error(NULL, MCP_ERR_PARSE, "parse error");
    else{
	if(c->owner == MCP_OWNER_AIMON && aimon_reply(c, req))
	  resp = NULL;
	else
	  resp = handle_request(req, c->id, c->owner, &queued);

	cJSON_Delete(req);
    }

    if(queued)
      c->outstanding++;

    if(resp){
	if(conn_send(c, resp) < 0)
	  c->read_closed = 1;		/* stop taking work; drop below */

	cJSON_free(resp);
    }
}


static void
conn_read(MCP_CONN_S *c)
{
    ssize_t n;
    char   *nl, *start;

    for(;;){
	if(c->rcap - c->rlen < 4096){
	    size_t ncap = c->rcap ? c->rcap * 2 : 8192;
	    char  *nb;

	    if(ncap > MCP_MAX_LINE + 2)
	      ncap = MCP_MAX_LINE + 2;

	    if(ncap <= c->rlen + 1){	/* a line longer than the cap */
		char *resp = mcp_make_error(NULL, MCP_ERR_INVALID_REQ,
					    "request line too long");
		if(resp){
		    (void) conn_send(c, resp);
		    cJSON_free(resp);
		}

		c->rlen = 0;
		c->read_closed = 1;
		return;
	    }

	    if((nb = realloc(c->rbuf, ncap)) == NULL){
		c->read_closed = 1;
		return;
	    }

	    c->rbuf = nb;
	    c->rcap = ncap;
	}

	n = read(c->fd, c->rbuf + c->rlen, c->rcap - c->rlen - 1);
	if(n < 0){
	    if(errno == EINTR)
	      continue;

	    if(errno != EAGAIN && errno != EWOULDBLOCK)
	      c->read_closed = 1;

	    return;
	}

	if(n == 0){
	    c->read_closed = 1;		/* half-close: answer, then close */
	    return;
	}

	c->rlen += (size_t) n;
	c->rbuf[c->rlen] = '\0';

	start = c->rbuf;
	while((nl = memchr(start, '\n', c->rlen - (size_t)(start - c->rbuf))) != NULL){
	    *nl = '\0';
	    conn_line(c, start, (size_t)(nl - start));
	    start = nl + 1;
	}

	if(start != c->rbuf){
	    c->rlen -= (size_t)(start - c->rbuf);
	    memmove(c->rbuf, start, c->rlen);
	}

	if(c->read_closed)
	  return;
    }
}


/* send finished responses to the connection that asked for them */
static void
deliver_done(void)
{
    MCP_ITEM_S *list, *it;
    int		i;

    pthread_mutex_lock(&q_mutex);
    list = done_head;
    done_head = done_tail = NULL;
    pthread_mutex_unlock(&q_mutex);

    while((it = list) != NULL){
	list = it->next;
	it->next = NULL;

	for(i = 0; i < nconns; i++)
	  if(conns[i].id == it->conn_id)
	    break;

	if(i < nconns){				/* waiter still here */
	    conns[i].outstanding--;
	    if(it->response && conn_send(&conns[i], it->response) < 0)
	      conns[i].read_closed = 1;
	}

	mcp_item_free(it);
    }
}


/*----------------------------------------------------------------------
    aimon gateway client, plan section 2.6.  Listener thread.  The
    gateway connection is one more entry in conns[], owned by
    MCP_OWNER_AIMON, so its lines, queueing and replies take the same
    path as a socket client.  Only registration is special.  aimon
    errors never reach the mail screens; the monitor screen shows the
    status text.
  ----*/
static long long
now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return((long long) ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}


static void
aimon_set_status(const char *fmt, ...)
{
    va_list ap;

    pthread_mutex_lock(&q_mutex);
    va_start(ap, fmt);
    vsnprintf(aimon_status, sizeof(aimon_status), fmt, ap);
    va_end(ap);
    pthread_mutex_unlock(&q_mutex);
}


void
mcp_aimon_status(char *buf, size_t len)
{
    if(buf == NULL || len == 0)
      return;

    pthread_mutex_lock(&q_mutex);
    snprintf(buf, len, "%s", aimon_status);
    pthread_mutex_unlock(&q_mutex);
}


/* any thread */
int
mcp_aimon_connected(void)
{
    int connected;

    pthread_mutex_lock(&q_mutex);
    connected = (strstr(aimon_status, "registered") != NULL);
    pthread_mutex_unlock(&q_mutex);
    return(connected);
}


/*
 * Main thread, after the monitor screen changed aimon.enabled (drop 0)
 * or asked for a fresh registration (drop 1).
 */
void
mcp_aimon_kick(int drop)
{
    int want = mcp_aimon_enabled();

    pthread_mutex_lock(&q_mutex);
    if(drop)
      aimon_drop_req = 1;

    /* what the listener is about to do, so a screen redrawn now is right */
    if(!want)
      snprintf(aimon_status, sizeof(aimon_status), "off");
    else if(drop || !strcmp(aimon_status, "off"))
      snprintf(aimon_status, sizeof(aimon_status), "connecting to aimon");

    pthread_mutex_unlock(&q_mutex);
    pipe_poke(lwake_pipe[1]);
}


/* wait 1, 2, 4, 8, 16, then 30 seconds between attempts */
static void
aimon_schedule_retry(const char *why)
{
    int last = (int) (sizeof(aimon_backoff_s) / sizeof(aimon_backoff_s[0])) - 1;
    int secs = aimon_backoff_s[aimon_backoff];

    if(aimon_backoff < last)
      aimon_backoff++;

    aimon_state = AIMON_WAIT;
    aimon_deadline = now_ms() + 1000LL * secs;
    aimon_set_status("not connected: %.100s; retry in %ds",
		     why ? why : "unknown", secs);
}


/* conn_close ran on the aimon connection */
static void
aimon_conn_gone(void)
{
    if(aimon_closing)
      return;

    aimon_schedule_retry(aimon_why[0] ? aimon_why : "connection closed");
    aimon_why[0] = '\0';
}


/* close whatever stage the aimon connection is in; no retry is scheduled */
static void
aimon_teardown(void)
{
    int i;

    if(aimon_fd >= 0){
	close(aimon_fd);
	aimon_fd = -1;
    }

    aimon_closing = 1;
    for(i = nconns - 1; i >= 0; i--)
      if(conns[i].owner == MCP_OWNER_AIMON)
	conn_close(i);

    aimon_closing = 0;
}


/*
 * {"jsonrpc":"2.0","id":1,"method":"gateway/register","params":{...}}
 * Returns a cJSON_free'd line without newline, or NULL.
 */
static char *
aimon_register_line(void)
{
    cJSON *req, *params, *list, *tools;
    char  *s;

    if((list = mcp_tools_list_json()) == NULL)
      return(NULL);

    tools = cJSON_DetachItemFromObjectCaseSensitive(list, "tools");
    cJSON_Delete(list);
    if(tools == NULL)
      return(NULL);

    if((req = cJSON_CreateObject()) == NULL){
	cJSON_Delete(tools);
	return(NULL);
    }

    params = NULL;
    if(!cJSON_AddStringToObject(req, "jsonrpc", "2.0")
       || !cJSON_AddNumberToObject(req, "id", 1)
       || !cJSON_AddStringToObject(req, "method", "gateway/register")
       || (params = cJSON_AddObjectToObject(req, "params")) == NULL
       || !cJSON_AddStringToObject(params, "subsystem", MCP_SERVER_NAME)
       || !cJSON_AddStringToObject(params, "version", PACKAGE_VERSION)
       || !cJSON_AddStringToObject(params, "display_name", "Alpine Mail")
       || !cJSON_AddStringToObject(params, "short_name", "Alpine")
       || !cJSON_AddNumberToObject(params, "priority", 40)
       || !cJSON_AddItemToObject(params, "tools", tools)){
	cJSON_Delete(tools);            /* not yet owned by params */
	cJSON_Delete(req);
	return(NULL);
    }

    s = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    return(s);
}


/*
 * A line from aimon with no method: the answer to gateway/register.
 * Returns 1 when the line was consumed here.
 */
static int
aimon_reply(MCP_CONN_S *c, cJSON *msg)
{
    cJSON *id, *res, *st;

    if(cJSON_GetObjectItemCaseSensitive(msg, "method"))
      return(0);

    res = cJSON_GetObjectItemCaseSensitive(msg, "result");
    if(res == NULL && cJSON_GetObjectItemCaseSensitive(msg, "error") == NULL)
      return(0);

    id = cJSON_GetObjectItemCaseSensitive(msg, "id");
    if(!c->reg_pending || !cJSON_IsNumber(id) || id->valuedouble != 1)
      return(1);                        /* unknown id: ignored */

    c->reg_pending = 0;
    st = cJSON_IsObject(res) ? cJSON_GetObjectItemCaseSensitive(res, "status")
			     : NULL;
    if(cJSON_IsString(st) && !strcmp(st->valuestring, "registered")){
	aimon_state = AIMON_UP;
	aimon_backoff = 0;
	aimon_why[0] = '\0';
	aimon_set_status("registered with aimon");
    }
    else{
	snprintf(aimon_why, sizeof(aimon_why), "registration refused (%.40s)",
		 cJSON_IsString(st) ? st->valuestring : "no status");
	c->read_closed = 1;             /* closed by the listener loop */
    }

    return(1);
}


/* the TCP connection is up; send the registration */
static void
aimon_connected(void)
{
    MCP_CONN_S *c;
    char       *line;
    int         fd = aimon_fd;

    aimon_fd = -1;

    if(nconns >= MCP_MAX_CONN){
	close(fd);
	aimon_schedule_retry("no connection slot");
	return;
    }

    if((line = aimon_register_line()) == NULL){
	close(fd);
	aimon_schedule_retry("cannot build registration");
	return;
    }

    c = &conns[nconns];
    memset(c, 0, sizeof(*c));
    c->fd = fd;
    c->id = next_conn_id++;
    c->owner = MCP_OWNER_AIMON;
    c->reg_pending = 1;
    nconns++;

    if(conn_send(c, line) < 0 || conn_flush(c) < 0){
	cJSON_free(line);
	snprintf(aimon_why, sizeof(aimon_why), "cannot send registration");
	conn_close(nconns - 1);         /* schedules the retry */
	return;
    }

    cJSON_free(line);
    aimon_state = AIMON_REGISTERING;
    aimon_deadline = now_ms() + MCP_AIMON_TIMEOUT_MS;
    aimon_set_status("registering with aimon");
}


/* host may be an IP address or a DNS hostname */
static void
aimon_start_connect(void)
{
    struct addrinfo hints, *ai = NULL, *cur, *pick;
    char            host[128], svc[16];
    int             port, fd, rc, one = 1, err;

    if(mcp_aimon_target(host, sizeof(host), &port) < 0
       || port < 1 || port > 65535){
	aimon_schedule_retry("aimon host or port in config.json is invalid");
	return;
    }

    snprintf(svc, sizeof(svc), "%d", port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICSERV;
    if((rc = getaddrinfo(host, svc, &hints, &ai)) != 0 || ai == NULL){
	if(ai)
	  freeaddrinfo(ai);
	aimon_schedule_retry(rc != 0 ? gai_strerror(rc) : "cannot resolve aimon host");
	return;
    }

    pick = ai;
    for(cur = ai; cur != NULL; cur = cur->ai_next){
	if(cur->ai_family == AF_INET){
	    pick = cur;
	    break;
	}
    }

    fd = socket(pick->ai_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
		pick->ai_protocol);
    if(fd < 0){
	err = errno;
	freeaddrinfo(ai);
	aimon_schedule_retry(strerror(err));
	return;
    }

    (void) setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    rc = connect(fd, pick->ai_addr, pick->ai_addrlen);
    err = errno;
    freeaddrinfo(ai);

    if(rc == 0){
	aimon_fd = fd;
	aimon_connected();
	return;
    }

    if(err == EINPROGRESS){
	aimon_fd = fd;
	aimon_state = AIMON_CONNECTING;
	aimon_deadline = now_ms() + MCP_AIMON_TIMEOUT_MS;
	aimon_set_status("connecting to aimon");
	return;
    }

    close(fd);
    aimon_schedule_retry(strerror(err));
}


/*
 * Once per listener pass: apply enable / disable / drop requests and
 * run the deadlines.  Connection and read events are handled in the
 * main loop.
 */
static void
aimon_tick(void)
{
    int       want, drop;
    long long now = now_ms();

    pthread_mutex_lock(&q_mutex);
    drop = aimon_drop_req;
    aimon_drop_req = 0;
    pthread_mutex_unlock(&q_mutex);

    want = mcp_aimon_enabled();

    if(!want || drop){
	if(aimon_state != AIMON_OFF)
	  aimon_teardown();

	aimon_backoff = 0;
	aimon_why[0] = '\0';
	if(!want){
	    aimon_state = AIMON_OFF;
	    aimon_set_status("off");
	    return;
	}

	aimon_state = AIMON_WAIT;       /* drop: register again now */
	aimon_deadline = now;
    }

    if(aimon_state == AIMON_OFF){
	aimon_state = AIMON_WAIT;
	aimon_deadline = now;
    }

    switch(aimon_state){
      case AIMON_WAIT :
	if(now >= aimon_deadline)
	  aimon_start_connect();

	break;

      case AIMON_CONNECTING :
      case AIMON_REGISTERING :
	if(now >= aimon_deadline){
	    int connecting = (aimon_state == AIMON_CONNECTING);

	    aimon_teardown();
	    aimon_schedule_retry(connecting ? "connect timed out"
					    : "no registration reply");
	}

	break;

      default :
	break;
    }
}


static void *
mcp_listener_main(void *arg)
{
    struct pollfd pfd[3 + MCP_MAX_CONN];
    int		  i, n, stop, ci, timeout;
    long long	  d;

    (void) arg;

    for(i = 0; i < MCP_MAX_CONN; i++)
      conns[i].fd = -1;

    for(;;){
	aimon_tick();

	n = 0;
	pfd[n].fd = lwake_pipe[0];
	pfd[n].events = POLLIN;
	pfd[n++].revents = 0;
	pfd[n].fd = listen_fd;
	pfd[n].events = (nconns < MCP_MAX_CONN - 1) ? POLLIN : 0;
	pfd[n++].revents = 0;
	for(i = 0; i < nconns; i++){
	    pfd[n].fd = conns[i].fd;
	    pfd[n].events = (conns[i].read_closed ? 0 : POLLIN)
			    | (conns[i].wlen ? POLLOUT : 0);
	    pfd[n++].revents = 0;
	}

	/* the aimon connect in flight, if any, is the last entry */
	ci = -1;
	if(aimon_state == AIMON_CONNECTING && aimon_fd >= 0){
	    ci = n;
	    pfd[n].fd = aimon_fd;
	    pfd[n].events = POLLOUT;
	    pfd[n++].revents = 0;
	}

	/* sleep until the next aimon deadline, or until something happens */
	timeout = -1;
	if(aimon_state == AIMON_WAIT || aimon_state == AIMON_CONNECTING
	   || aimon_state == AIMON_REGISTERING){
	    d = aimon_deadline - now_ms();
	    if(d < 0)
	      d = 0;

	    if(d > 60000)
	      d = 60000;

	    timeout = (int) d + 1;
	}

	if(poll(pfd, (nfds_t) n, timeout) < 0){
	    if(errno == EINTR)
	      continue;

	    srv_set_error("listener poll: %s", strerror(errno));
	    break;
	}

	if(pfd[0].revents & POLLIN){
	    pipe_drain(lwake_pipe[0]);
	    pthread_mutex_lock(&q_mutex);
	    stop = stop_listener;
	    pthread_mutex_unlock(&q_mutex);
	    if(stop)
	      break;

	    deliver_done();
	}

	/* walk existing connections first; accept may reorder conns[] */
	for(i = nconns - 1; i >= 0; i--){
	    MCP_CONN_S *c = &conns[i];
	    short	re = pfd[2 + i].revents;

	    if(re & (POLLERR | POLLNVAL)){
		conn_close(i);
		continue;
	    }

	    if(re & POLLIN)
	      conn_read(c);

	    /*
	     * POLLHUP on a Unix stream socket means the peer closed both
	     * directions.  A half-close only shows up as EOF on read.
	     */
	    if(re & POLLHUP){
		conn_close(i);
		continue;
	    }

	    if(c->wlen && conn_flush(c) < 0){
		conn_close(i);
		continue;
	    }

	    /*
	     * A gateway connection that hit EOF is closed at once: its
	     * queued calls are cancelled and the reconnect loop takes over.
	     */
	    if(c->read_closed
	       && (c->owner == MCP_OWNER_AIMON
		   || (c->outstanding == 0 && c->wlen == 0)))
	      conn_close(i);
	}

	if(ci >= 0 && pfd[ci].revents){
	    int	      err = 0;
	    socklen_t el = sizeof(err);

	    if(getsockopt(aimon_fd, SOL_SOCKET, SO_ERROR, &err, &el) < 0)
	      err = errno;

	    if(err == 0 && (pfd[ci].revents & POLLOUT))
	      aimon_connected();
	    else{
		close(aimon_fd);
		aimon_fd = -1;
		aimon_schedule_retry(err ? strerror(err) : "connection failed");
	    }
	}

	if(pfd[1].revents & POLLIN){
	    int		  fd;
	    struct ucred  cred;
	    socklen_t	  clen = sizeof(cred);

	    fd = accept4(listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
	    if(fd >= 0){
		if(getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &clen) < 0
		   || cred.uid != getuid() || nconns >= MCP_MAX_CONN - 1){
		    close(fd);
		}
		else{
		    memset(&conns[nconns], 0, sizeof(MCP_CONN_S));
		    conns[nconns].fd = fd;
		    conns[nconns].id = next_conn_id++;
		    conns[nconns].owner = MCP_OWNER_SOCKET;
		    nconns++;
		}
	    }
	}
    }

    while(nconns > 0)
      conn_close(nconns - 1);

    if(aimon_fd >= 0){
	close(aimon_fd);
	aimon_fd = -1;
    }

    return(NULL);
}


/*----------------------------------------------------------------------
    Start and stop, main thread
  ----*/
int
mcp_server_start(void)
{
    struct sockaddr_un sun;
    struct stat	       sb;
    sigset_t	       all, old;
    mode_t	       oldmask;
    int		       fd, rc;

    if(srv_running || !mcp_enabled())
      return(0);

    /* a server started again in the same process begins from clean state */
    pthread_mutex_lock(&q_mutex);
    stop_listener = 0;
    aimon_drop_req = 0;
    snprintf(aimon_status, sizeof(aimon_status), "off");
    pthread_mutex_unlock(&q_mutex);
    aimon_state = AIMON_OFF;
    aimon_backoff = 0;
    aimon_closing = 0;
    nconns = 0;

    if(mcp_build_socket_path(sock_path, sizeof(sock_path)) < 0){
	srv_set_error("socket path too long");
	return(-1);
    }

    if(pipe2(wake_pipe, O_NONBLOCK | O_CLOEXEC) < 0
       || pipe2(lwake_pipe, O_NONBLOCK | O_CLOEXEC) < 0){
	srv_set_error("pipe2: %s", strerror(errno));
	goto fail;
    }

    memset(&sun, 0, sizeof(sun));
    sun.sun_family = AF_UNIX;
    memcpy(sun.sun_path, sock_path, strlen(sock_path) + 1);

    /* an old socket file: in use by a live Alpine, or left by a crash */
    if(lstat(sock_path, &sb) == 0){
	if(!S_ISSOCK(sb.st_mode) || sb.st_uid != getuid()){
	    srv_set_error("%s exists and is not our socket", sock_path);
	    goto fail;
	}

	if((fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)) >= 0){
	    rc = connect(fd, (struct sockaddr *) &sun, sizeof(sun));
	    close(fd);
	    if(rc == 0){
		srv_set_error("another Alpine is serving %s", sock_path);
		goto fail;
	    }
	}

	if(unlink(sock_path) < 0){
	    srv_set_error("unlink stale socket: %s", strerror(errno));
	    goto fail;
	}
    }

    if((listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)) < 0){
	srv_set_error("socket: %s", strerror(errno));
	goto fail;
    }

    oldmask = umask(077);
    rc = bind(listen_fd, (struct sockaddr *) &sun, sizeof(sun));
    umask(oldmask);
    if(rc < 0){
	srv_set_error("bind %s: %s", sock_path, strerror(errno));
	goto fail;
    }

    if(chmod(sock_path, 0600) < 0 || lstat(sock_path, &sb) < 0){
	srv_set_error("chmod %s: %s", sock_path, strerror(errno));
	unlink(sock_path);
	goto fail;
    }

    sock_dev = sb.st_dev;
    sock_ino = sb.st_ino;

    if(listen(listen_fd, 8) < 0 || set_nonblock(listen_fd) < 0){
	srv_set_error("listen: %s", strerror(errno));
	unlink(sock_path);
	goto fail;
    }

    /* Alpine's signal handlers belong to the main thread only */
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, &old);
    rc = pthread_create(&listener_tid, NULL, mcp_listener_main, NULL);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    if(rc != 0){
	srv_set_error("pthread_create: %s", strerror(rc));
	unlink(sock_path);
	goto fail;
    }

    srv_running = 1;
    srv_set_error("%s", "");
    return(0);

  fail:
    if(listen_fd >= 0){
	close(listen_fd);
	listen_fd = -1;
    }

    for(fd = 0; fd < 2; fd++){
	if(wake_pipe[fd] >= 0){
	    close(wake_pipe[fd]);
	    wake_pipe[fd] = -1;
	}

	if(lwake_pipe[fd] >= 0){
	    close(lwake_pipe[fd]);
	    lwake_pipe[fd] = -1;
	}
    }

    return(-1);
}


void
mcp_server_stop(void)
{
    MCP_ITEM_S *it;
    struct stat sb;
    int		i;

    if(!srv_running)
      return;

    pthread_mutex_lock(&q_mutex);
    stop_listener = 1;
    pthread_mutex_unlock(&q_mutex);
    pipe_poke(lwake_pipe[1]);
    pthread_join(listener_tid, NULL);

    /* remove the socket only if it is still the one we made */
    if(lstat(sock_path, &sb) == 0 && sb.st_dev == sock_dev
       && sb.st_ino == sock_ino)
      unlink(sock_path);

    close(listen_fd);
    listen_fd = -1;

    pthread_mutex_lock(&q_mutex);
    while((it = q_head) != NULL){
	q_head = it->next;
	mcp_item_free(it);
    }

    q_tail = NULL;
    q_len = 0;
    while((it = done_head) != NULL){
	done_head = it->next;
	mcp_item_free(it);
    }

    done_tail = NULL;

    /* calls still waiting for the human's answer go with the server */
    while((it = park_head) != NULL){
	park_head = it->next;
	mcp_item_free(it);
    }

    park_tail = NULL;
    park_len = 0;
    park_new = 0;
    asking = NULL;
    pthread_mutex_unlock(&q_mutex);

    for(i = 0; i < 2; i++){
	close(wake_pipe[i]);
	close(lwake_pipe[i]);
	wake_pipe[i] = lwake_pipe[i] = -1;
    }

    srv_running = 0;
}

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

#ifndef PITH_MCP_MCPINT_INCLUDED
#define PITH_MCP_MCPINT_INCLUDED

/*
 * Internal interfaces shared by the files in pith/mcp/.
 */

#include <stddef.h>
#include "mcpjson.h"


/*
 * Outcome of one tool run on the main thread.  code 0 means success
 * and result holds the structured answer; otherwise code is a JSON-RPC
 * error code from mcp.h and message says why.  summary and folder are
 * optional short texts describing the call (no bodies, subjects, search
 * terms or credentials); they are kept for logs and tests.
 */
typedef struct mcp_call {
    int	   code;
    char   message[512];
    cJSON *result;
    char   summary[96];		/* e.g. "12 found" */
    char   folder[256];		/* folder the call used, if any */
    int	   park;		/* set by run: wait for the human, see below */
    char   park_prompt[400];	/* the yes/no question when park is set */
} MCP_CALL_S;


/*
 * One tool.  validate runs on the listener or aimon thread and must
 * only look at args.  run runs on Alpine's main thread inside the
 * drain, with mcp_noninteractive() true.
 */
typedef struct mcp_tool {
    const char *name;
    const char *description;
    const char *input_schema;		/* JSON text */
    int	      (*validate)(cJSON *, char *, size_t);
    void      (*run)(cJSON *, MCP_CALL_S *);
    const char *activity;		/* short verb, e.g. "list folders" */
    void      (*run_confirmed)(cJSON *, MCP_CALL_S *);
					/* NULL, or the part that runs after
					   the human has said yes */
} MCP_TOOL_S;


/* mcptools.c */
#define MCP_UID_BATCH_MAX	250
#define MCP_FOLDER_BATCH_MAX	32
#define MCP_APPEND_BATCH_MAX	25

enum {
    MCP_UID_GONE = 0,
    MCP_UID_MATCH,
    MCP_UID_DIFFER
};

enum {
    MCP_ACT_DONE = 0,
    MCP_ACT_REISSUE,
    MCP_ACT_FAIL
};

typedef struct mcp_uids {
    unsigned long uid[MCP_UID_BATCH_MAX];
    size_t	  n;
    unsigned long missing[MCP_UID_BATCH_MAX];
    size_t	  nmissing;
    char	  set[4096];
} MCP_UIDS_S;

MCP_TOOL_S *mcp_tool_lookup(const char *);
cJSON	   *mcp_tools_list_json(void);
long	    mcp_uid_set(const unsigned long *, size_t, char *, size_t);
int	    mcp_batch_classify(const int *, size_t, int, size_t *, size_t *, size_t *);

/* mcpserv.c: per-call state written by the c-client callbacks */
void	mcp_call_error(MCP_CALL_S *, int, const char *, ...);
int	mcp_cur_login_needed(char *, size_t);
int	mcp_cur_cclient_error(char *, size_t);
void	mcp_cur_reset(void);

/* mcpserv.c: JSON-RPC line builders, any thread, free with cJSON_free */
char   *mcp_make_result(cJSON *, cJSON *);
char   *mcp_make_error(cJSON *, int, const char *);

/* mcppath.c */
int	mcp_build_socket_path(char *, size_t);

/* mcpmail.c, main thread */
void	mcp_run_list_folders(cJSON *, MCP_CALL_S *);
int	mcp_mailbox_known(const char *);

/* mcpread.c, main thread: shared with mcpwrite.c */
struct mail_stream	*mcp_acquire(const char *, int *, MCP_CALL_S *);
void	mcp_release(struct mail_stream *, int);
int	mcp_uid_ok(struct mail_stream *, cJSON *, MCP_CALL_S *, unsigned long *);
char   *mcp_clean_utf8(const char *, size_t, size_t);
int	mcp_uids_prepare(struct mail_stream *, cJSON *, MCP_CALL_S *,
			 MCP_UIDS_S *, const char *, int, int *);

/* mcpidem.c, main thread */
#define MCP_IDEM_ERROR		(-1)
#define MCP_IDEM_NEW		0	/* pending written and synced */
#define MCP_IDEM_DONE		1	/* same key, same arguments, finished */
#define MCP_IDEM_PENDING	2	/* an earlier attempt never finished */
#define MCP_IDEM_MISMATCH	3	/* same key, different arguments */
void	mcp_sha256_hex(const void *, size_t, char *);
int	mcp_idem_begin(const char *, const char *, const char *, char **);
int	mcp_idem_check(const char *, const char *, const char *, char **);
int	mcp_idem_commit(const char *, const char *, const char *, const char *);
void	mcp_idem_abort(const char *);

/* mcpwrite.c, main thread */
void	mcp_run_copy(cJSON *, MCP_CALL_S *);
void	mcp_run_set_flag(cJSON *, MCP_CALL_S *);
void	mcp_run_create_draft(cJSON *, MCP_CALL_S *);
void	mcp_run_send(cJSON *, MCP_CALL_S *);
void	mcp_run_send_confirmed(cJSON *, MCP_CALL_S *);

/* mcpread.c, main thread */
void	mcp_run_folder_status(cJSON *, MCP_CALL_S *);
void	mcp_run_list_messages(cJSON *, MCP_CALL_S *);
void	mcp_run_search(cJSON *, MCP_CALL_S *);
void	mcp_run_get_message(cJSON *, MCP_CALL_S *);
void	mcp_run_search_addressbook(cJSON *, MCP_CALL_S *);

#endif /* PITH_MCP_MCPINT_INCLUDED */

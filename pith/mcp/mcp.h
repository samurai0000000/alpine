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

#ifndef PITH_MCP_MCP_INCLUDED
#define PITH_MCP_MCP_INCLUDED

/*
 * Public face of the MCP mail platform.  Everything here is inert
 * unless config.json sets "mcp.enabled": mcp_server_start returns
 * without opening anything, mcp_wake_fd returns -1, and mcp_drain and
 * mcp_noninteractive do nothing.
 */

/* JSON-RPC and MCP error codes, plan section 2.4 */
#define MCP_ERR_PARSE		(-32700)
#define MCP_ERR_INVALID_REQ	(-32600)
#define MCP_ERR_NO_METHOD	(-32601)
#define MCP_ERR_PARAMS		(-32602)
#define MCP_ERR_INTERNAL	(-32603)
#define MCP_ERR_LOCKED		(-32001)
#define MCP_ERR_CANCELLED	(-32004)
#define MCP_ERR_QUEUE_FULL	(-32005)
#define MCP_ERR_IDEMPOTENCY	(-32006)
#define MCP_ERR_DECLINED	(-32007)
#define MCP_ERR_NOT_AUTH	(-32008)

#define MCP_PROTOCOL_VERSION	"2025-11-25"
#define MCP_SERVER_NAME		"alpine-mcp"
#define MCP_QUEUE_MAX		8

/* main thread only */
int	mcp_server_start(void);
void	mcp_server_stop(void);
int	mcp_server_running(void);
int	mcp_wake_fd(void);
void	mcp_drain(void);
int	mcp_noninteractive(void);

/*
 * A tool may ask the human before it acts (an off-list mail_send).  The
 * drain does not ask: it parks the call.  mcp_confirm_pending is called
 * from Alpine's command loop, where a question is allowed, with a
 * function that asks it and returns non-zero for yes.  Main thread.
 */
void	mcp_confirm_pending(int (*ask)(const char *));
int	mcp_pending_confirms(void);
int	mcp_take_new_park(void);	/* 1 once per newly parked call */

/* c-client callbacks report here while a tool runs (main thread) */
void	mcp_note_login_needed(char *);
void	mcp_note_cclient_msg(char *, long);

/*
 * Idempotency records still "pending" (an attempt that never finished).
 * mcp_idem_pending returns malloc'd names (free each, then the array);
 * mcp_idem_delete_pending removes one, only if it is still pending.
 * Main thread.  Used by the monitor screen.
 */
int	mcp_idem_pending(char ***, int *);
int	mcp_idem_delete_pending(const char *);

/* any thread */
void	mcp_server_last_error(char *, size_t);
char   *mcp_socket_path(char *, size_t);

/*
 * aimon gateway client (plan section 2.6).  mcp_aimon_status copies a
 * one-line state: "off", "connecting to aimon", "registered with
 * aimon", or "not connected: <why>; retry in <n>s".  mcp_aimon_kick
 * tells the listener to re-read aimon.enabled; with drop non-zero it
 * also closes the connection so the reconnect loop registers again.
 */
void	mcp_aimon_status(char *, size_t);
void	mcp_aimon_kick(int);

#endif /* PITH_MCP_MCP_INCLUDED */

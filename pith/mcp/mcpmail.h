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

#ifndef PITH_MCP_MCPMAIL_INCLUDED
#define PITH_MCP_MCPMAIL_INCLUDED

/*
 * Hooks the c-client login callbacks in pith/imap.c use while an MCP
 * tool runs.  Include after the c-client headers (NETMBX).
 */
void	mcp_login_cached(NETMBX *, char *, char **, long);
void	mcp_login_method_refused(NETMBX *);

#endif /* PITH_MCP_MCPMAIL_INCLUDED */

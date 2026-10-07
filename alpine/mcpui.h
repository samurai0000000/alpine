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

#ifndef ALPINE_MCPUI_INCLUDED
#define ALPINE_MCPUI_INCLUDED

/*
 * Alpine's side of the MCP platform: the main-thread drain and the
 * question for an off-list send.  All of it does nothing when MCP is
 * disabled.
 */
void	mcp_ui_init(void);	/* after mcp_server_start */
void	mcp_ui_drain(void);	/* run queued tools now */
void	mcp_ui_confirm(void);	/* put parked questions to the human */
struct pine;
void	mcp_monitor_screen(struct pine *);	/* Setup / monItor */

#endif /* ALPINE_MCPUI_INCLUDED */

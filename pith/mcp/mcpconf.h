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

#ifndef PITH_MCP_MCPCONF_INCLUDED
#define PITH_MCP_MCPCONF_INCLUDED

/*
 * MCP policy file: $HOME/.config/alpine-mcp/config.json, mode 0600.
 *
 * A missing, unreadable, insecure or invalid file means the defaults
 * below, and the defaults mean upstream Alpine: MCP off, aimon off,
 * operator lock off, empty recipient allow list.  None of this lives
 * in .pinerc.
 */

#define MCP_CONF_DIR		".config/alpine-mcp"
#define MCP_CONF_FILE		"config.json"
#define MCP_AIMON_DEF_HOST	"127.0.0.1"
#define MCP_AIMON_DEF_PORT	3885
#define MCP_ALLOW_MAX		1024	/* allow-list entries kept */
#define MCP_ADDR_MAX		320	/* longest allow-list entry */

typedef struct mcp_config {
    int    enabled;		/* "mcp.enabled", fixed at startup */
    int    operator_lock;	/* "operator_lock", guarded, see below */
    char **allowed;		/* "allowed_recipients", NULL terminated */
    int    aimon_enabled;	/* "aimon.enabled" */
    char  *aimon_host;		/* "aimon.host" */
    int    aimon_port;		/* "aimon.port" */
    int    file_ok;		/* config file was read and valid */
    char   last_error[256];	/* why the file was rejected, or "" */
} MCP_CONFIG_S;


/*
 * Exported prototypes.  mcp_config_load runs once on the main thread,
 * before the screen is configured.  The getters below are safe from
 * any thread.
 */
void	mcp_config_load(char *);
int	mcp_config_save(void);
int	mcp_enabled(void);
int	mcp_lock_engaged(void);
void	mcp_set_lock(int);
int	mcp_aimon_enabled(void);
void	mcp_set_aimon_enabled(int);
int	mcp_aimon_target(char *, size_t, int *);
int	mcp_allow_count(void);
int	mcp_recipient_allowed(char *);
int	mcp_allow_entry_valid(char *);
char  **mcp_allow_list_copy(void);
int	mcp_set_allow_list(char **, char *, size_t);
void	mcp_free_list(char ***);
char   *mcp_config_path(void);
void	mcp_config_last_error(char *, size_t);

#endif /* PITH_MCP_MCPCONF_INCLUDED */

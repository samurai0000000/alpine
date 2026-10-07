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

#ifndef PITH_MCP_MCPJSON_INCLUDED
#define PITH_MCP_MCPJSON_INCLUDED

#include "cJSON.h"

/*
 * cJSON keeps its last parse error in a static.  Every MCP thread
 * parses through this wrapper so that static is never written by two
 * threads at once.
 */
cJSON  *mcp_json_parse(const char *, size_t);

#endif /* PITH_MCP_MCPJSON_INCLUDED */

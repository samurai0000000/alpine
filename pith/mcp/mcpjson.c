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

#include <stddef.h>
#include <string.h>
#include <pthread.h>

#include "mcpjson.h"


static pthread_mutex_t mcp_json_mutex = PTHREAD_MUTEX_INITIALIZER;


/*
 * Parse len bytes of buf.  buf[len] must be '\0' and buf must not
 * contain a NUL before that.  Returns a cJSON tree the caller frees
 * with cJSON_Delete, or NULL if the bytes are not one complete JSON
 * value.  Trailing bytes after the value are rejected.
 */
cJSON *
mcp_json_parse(const char *buf, size_t len)
{
    cJSON *root;

    if(buf == NULL || len == 0 || buf[len] != '\0'
       || memchr(buf, '\0', len) != NULL)
      return(NULL);

    pthread_mutex_lock(&mcp_json_mutex);
    /* length includes the terminating NUL, as cJSON's check requires */
    root = cJSON_ParseWithLengthOpts(buf, len + 1, NULL, 1);
    pthread_mutex_unlock(&mcp_json_mutex);

    return(root);
}

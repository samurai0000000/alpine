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
 * Runtime socket path.  libc only, so that a client program can use the
 * same function and always agree with Alpine.
 *
 *   $XDG_RUNTIME_DIR/alpine-mcp.sock   when XDG_RUNTIME_DIR is set and
 *                                      is a directory we own and can
 *                                      write and search
 *   /tmp/alpine-mcp.<hostname>.sock    otherwise
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/un.h>

#ifndef HOST_NAME_MAX
#define HOST_NAME_MAX 255
#endif

int mcp_build_socket_path(char *, size_t);


/*
 * Fill buf with the socket path.  Returns 0, or -1 if the path does not
 * fit in buf or in sun_path.
 */
int
mcp_build_socket_path(char *buf, size_t len)
{
    struct sockaddr_un sun;
    struct stat	       sb;
    char	       host[HOST_NAME_MAX + 1], *p;
    const char	      *xdg;
    int		       n;

    if(buf == NULL || len == 0)
      return(-1);

    xdg = getenv("XDG_RUNTIME_DIR");
    if(xdg && *xdg == '/'
       && stat(xdg, &sb) == 0 && S_ISDIR(sb.st_mode)
       && sb.st_uid == getuid()
       && access(xdg, W_OK | X_OK) == 0){
	n = snprintf(buf, len, "%s/alpine-mcp.sock", xdg);
    }
    else{
	if(gethostname(host, sizeof(host)) != 0 || host[0] == '\0')
	  snprintf(host, sizeof(host), "localhost");

	host[sizeof(host) - 1] = '\0';
	for(p = host; *p; p++)		/* keep the name a single path part */
	  if(*p == '/' || *p <= ' ')
	    *p = '_';

	n = snprintf(buf, len, "/tmp/alpine-mcp.%s.sock", host);
    }

    if(n < 0 || (size_t) n >= len || (size_t) n >= sizeof(sun.sun_path)){
	buf[0] = '\0';
	return(-1);
    }

    return(0);
}

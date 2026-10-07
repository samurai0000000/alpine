/*
 * $Id: read.h 761 2007-10-23 22:35:18Z hubert@u.washington.edu $
 *
 * ========================================================================
 * Copyright 2006 University of Washington
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * ========================================================================
 */


#ifndef PICO_OSDEP_READ_INCLUDED
#define PICO_OSDEP_READ_INCLUDED


#include <general.h>


/* exported prototypes */
#ifndef _WINDOWS
UCS	input_ready(int);
int	read_one_char(void);

/*
 * Optional host hooks, NULL unless the host sets them (Alpine does when
 * MCP is enabled).  input_ready_wake_fd returns an extra descriptor
 * that may wake input_ready, or -1.  When only that descriptor is
 * readable, input_ready calls input_ready_wake_run and waits again.
 * After input_ready_wake_run,
 * a non-zero result from input_ready_wake_return makes input_ready return
 * NO_OP_COMMAND, as a timeout does, so that the host's command loop runs.
 */
extern int  (*input_ready_wake_fd)(void);
extern void (*input_ready_wake_run)(void);
extern int  (*input_ready_wake_return)(void);
#else /* _WINDOWS */
int set_time_of_last_input(void);
#endif /* _WINDOWS */

time_t	time_of_last_input(void);


#endif /* PICO_OSDEP_READ_INCLUDED */

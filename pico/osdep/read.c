/*
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

/*
 * Keyboard input test and read functions
 */

#include <system.h>		/* os-dep defs/includes */
#include <general.h>		/* generally useful definitions */

#include "../keydefs.h"

#ifndef _WINDOWS
#include "raw.h"
#endif /* !_WINDOWS */

#include "read.h"


static time_t _time_of_last_input;

#ifndef _WINDOWS
/*
 * Optional host hooks; NULL unless the host sets them (Alpine does when
 * MCP is enabled).  See read.h.
 */
int  (*input_ready_wake_fd)(void);
void (*input_ready_wake_run)(void);
int  (*input_ready_wake_return)(void);
int  (*input_ready_timer_check)(unsigned long *timeout_ms);
void (*input_ready_timer_tick)(void);
#endif /* !_WINDOWS */

time_t
time_of_last_input(void)
{
    if(_time_of_last_input == 0)
      _time_of_last_input = time((time_t *) 0);

    return(_time_of_last_input);
}

#ifndef _WINDOWS
#if	HAVE_SELECT

#ifdef	HAVE_SYS_SELECT_H
# include <sys/select.h>
#endif


/*
 *  Check whether or not a character is ready to be read, or time out.
 *  This version uses a select call.
 *
 *   Args: time_out -- number of seconds before it will time out.
 *
 * Result: NO_OP_IDLE:	  timed out before any chars ready, time_out > 25
 *         NO_OP_COMMAND: timed out before any chars ready, time_out <= 25
 *         READ_INTR:	  read was interrupted
 *         READY_TO_READ: input is available
 *         BAIL_OUT:	  reading input failed, time to bail gracefully
 *         PANIC_NOW:	  system call error, die now
 *
 *  If the host's wake descriptor (input_ready_wake_fd) is readable and
 *  stdin is not, input_ready_wake_run is called and the wait resumes
 *  with whatever is left of time_out.
 */
UCS
input_ready(int time_out)
{
     struct timeval tmo;
     fd_set         readfds, errfds;
     int	    res, wfd, maxfd, drained = 0;
     time_t	    deadline = 0, left;
     unsigned long  spin_ms = 0;
     int            timer_active;

     fflush(stdout);

     if(time_out > 0)
       deadline = time((time_t *) 0) + time_out;

     for(;;){
	 wfd = (input_ready_wake_fd && input_ready_wake_run)
		 ? (*input_ready_wake_fd)() : -1;

	 timer_active = (input_ready_timer_check && (*input_ready_timer_check)(&spin_ms));

	 if(time_out <= 0 && wfd < 0 && !timer_active)
	   break;			/* no timeout: the read blocks */

         /* Check to see if there are bytes to read with a timeout */
	 FD_ZERO(&readfds);
	 FD_ZERO(&errfds);
	 FD_SET(STDIN_FD, &readfds);
	 FD_SET(STDIN_FD, &errfds);
	 maxfd = STDIN_FD;
	 if(wfd >= 0){
	     FD_SET(wfd, &readfds);
	     if(wfd > maxfd)
	       maxfd = wfd;
	 }

	 if(time_out > 0 || timer_active){
	     unsigned long wait_ms;
	     if(time_out > 0){
		 left = drained ? deadline - time((time_t *) 0) : time_out;
		 wait_ms = (left > 0) ? (unsigned long)(left * 1000) : 0;
		 if(timer_active && spin_ms < wait_ms)
		   wait_ms = spin_ms;
	     }
	     else{
		 wait_ms = spin_ms;
	     }
	     tmo.tv_sec  = wait_ms / 1000;
	     tmo.tv_usec = (wait_ms % 1000) * 1000;
	 }

         res = select(maxfd+1, &readfds, 0, &errfds,
		      (time_out > 0 || timer_active) ? &tmo : NULL);
         if(res < 0){
             if(errno == EINTR || errno == EAGAIN)
               return(READ_INTR);

	     return(BAIL_OUT);
         }

         if(res == 0){ /* the select timed out */
#ifndef __CYGWIN__
	     if(getppid() == 1){
		 /* Parent is init! */
	         return(BAIL_OUT);
	     }
#endif
	     if(timer_active && input_ready_timer_tick){
		 (*input_ready_timer_tick)();
	     }

	     if(time_out > 0 && time((time_t *) 0) >= deadline){
		 return(time_out < IDLE_TIMEOUT ? NO_OP_COMMAND : NO_OP_IDLE);
	     }

	     drained = 1;
	     continue;
         }

	 /* only the host's descriptor: run its work, then wait again */
	 if(wfd >= 0 && FD_ISSET(wfd, &readfds)
	    && !FD_ISSET(STDIN_FD, &readfds) && !FD_ISSET(STDIN_FD, &errfds)){
	     (*input_ready_wake_run)();
	     drained = 1;

	     /*
	      * The host asked to be taken back to its command loop (a
	      * question for the human is waiting).  Return as a timeout
	      * does, so no key is eaten and read_command_prep runs.
	      */
	     if(input_ready_wake_return && (*input_ready_wake_return)())
	       return(NO_OP_COMMAND);

	     continue;
	 }

	 break;
     }

     _time_of_last_input = time((time_t *) 0);
     return(READY_TO_READ);
}


#elif	HAVE_POLL


#ifdef	HAVE_STROPTS_H
# include <stropts.h>
#endif

#ifdef	HAVE_SYS_POLL_H
# include <poll.h>
#endif

/*
 *  Check whether or not a character is ready to be read, or time out.
 *  This version uses a poll call.
 *
 *   Args: time_out -- number of seconds before it will time out.
 *
 * Result: NO_OP_IDLE:	  timed out before any chars ready, time_out > 25
 *         NO_OP_COMMAND: timed out before any chars ready, time_out <= 25
 *         READ_INTR:	  read was interrupted
 *         READY_TO_READ: input is available
 *         BAIL_OUT:	  reading input failed, time to bail gracefully
 *         PANIC_NOW:	  system call error, die now
 *
 *  If the host's wake descriptor (input_ready_wake_fd) is readable and
 *  stdin is not, input_ready_wake_run is called and the wait resumes
 *  with whatever is left of time_out.
 */
UCS
input_ready(int time_out)
{
     struct pollfd pollfd[2];
     int	   res, wfd, nfds, ms, drained = 0;
     time_t	   deadline = 0, left;
     unsigned long spin_ms = 0;
     int           timer_active;

     fflush(stdout);

     if(time_out > 0)
       deadline = time((time_t *) 0) + time_out;

     for(;;){
	 wfd = (input_ready_wake_fd && input_ready_wake_run)
		 ? (*input_ready_wake_fd)() : -1;

	 timer_active = (input_ready_timer_check && (*input_ready_timer_check)(&spin_ms));

	 if(time_out <= 0 && wfd < 0 && !timer_active)
	   break;			/* no timeout: the read blocks */

         /* Check to see if there are bytes to read with a timeout */
	 pollfd[0].fd = STDIN_FD;
	 pollfd[0].events = POLLIN;
	 pollfd[0].revents = 0;
	 nfds = 1;
	 if(wfd >= 0){
	     pollfd[1].fd = wfd;
	     pollfd[1].events = POLLIN;
	     pollfd[1].revents = 0;
	     nfds = 2;
	 }

	 if(time_out > 0 || timer_active){
	     if(time_out > 0){
		 left = drained ? deadline - time((time_t *) 0) : time_out;
		 ms = (left > 0) ? (int)(left * 1000) : 0;
		 if(timer_active && (int)spin_ms < ms)
		   ms = (int)spin_ms;
	     }
	     else{
		 ms = (int)spin_ms;
	     }
	 }
	 else
	   ms = -1;

	 res = poll (pollfd, nfds, ms);
	 if(res >= 0){				/* status bits OK? */
	     if(pollfd[0].revents & (POLLERR | POLLNVAL))
	       res = -1;			/* bad news, exit below! */
	     else if(pollfd[0].revents & POLLHUP)
	       return(BAIL_OUT);
	 }

         if(res < 0){
             if(errno == EINTR || errno == EAGAIN)
               return(READ_INTR);

	     return(PANIC_NOW);
         }

         if(res == 0){ /* the select timed out */
	     if(getppid() == 1){
		 /* Parent is init! */
	         return(BAIL_OUT);
	     }

	     if(timer_active && input_ready_timer_tick){
		 (*input_ready_timer_tick)();
	     }

	     if(time_out > 0 && time((time_t *) 0) >= deadline){
		 return(time_out < IDLE_TIMEOUT ? NO_OP_COMMAND : NO_OP_IDLE);
	     }

	     drained = 1;
	     continue;
         }

	 /* only the host's descriptor: run its work, then wait again */
	 if(nfds == 2 && (pollfd[1].revents & POLLIN)
	    && !(pollfd[0].revents & POLLIN)){
	     (*input_ready_wake_run)();
	     drained = 1;

	     /*
	      * The host asked to be taken back to its command loop (a
	      * question for the human is waiting).  Return as a timeout
	      * does, so no key is eaten and read_command_prep runs.
	      */
	     if(input_ready_wake_return && (*input_ready_wake_return)())
	       return(NO_OP_COMMAND);

	     continue;
	 }

	 break;
     }

     _time_of_last_input = time((time_t *) 0);
     return(READY_TO_READ);
}

#endif /* HAVE_POLL */


/*
 * Read one character from STDIN.
 *
 * Result:           -- the single character read
 *         READ_INTR -- read was interrupted
 *         BAIL_OUT  -- read error of some sort
 */
int
read_one_char(void)
{
     int	    res;
     unsigned char  c;

     res = read(STDIN_FD, &c, 1);

     if(res <= 0){
	 /*
	  * Error reading from terminal!
	  * The only acceptable failure is being interrupted.  If so,
	  * return a value indicating such...
	  */
	 if(res < 0 && errno == EINTR)
	   return(READ_INTR);
	 else
	   return(BAIL_OUT);
     }

     return((int)c);
}

#else /* _WINDOWS */
int
set_time_of_last_input(void)
{
    _time_of_last_input = time(0L);
    return(0);
}
#endif /* _WINDOWS */

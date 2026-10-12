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
 * MCP glue for Alpine's screens (plan section 2.5).
 *
 *   - input_ready (pico/osdep/read.c) calls mcp_ui_drain when only the
 *     MCP wake pipe is readable.
 *   - read_command_prep calls mcp_ui_drain for work that arrived while
 *     Alpine was busy, and mcp_ui_confirm to put an off-list send to
 *     the human.
 *
 * The screen is exactly upstream Alpine's: nothing here paints on it.
 * (An AI activity row, plan section 2.3.1, was built and removed.)
 */

#include "headers.h"
#include "mcpui.h"
#include "../pith/mcp/mcp.h"
#include "../pith/mcp/mcpconf.h"
#include "../pico/osdep/read.h"
#include "mailview.h"
#include "keymenu.h"
#include "reply.h"
#include "status.h"
#include "radio.h"
#include "../pith/helptext.h"
#include "../pith/store.h"
#include "titlebar.h"

#include <sys/ioctl.h>

#if	defined(SIGWINCH) && defined(TIOCGWINSZ)
extern jmp_buf winch_state;			/* alpine/osdep/termin.unx.c */
extern int     ready_for_winch, winch_occured;
#endif


void
mcp_ui_init(void)
{
    if(!mcp_enabled())
      return;

    input_ready_wake_return = mcp_take_new_park;
    if(mcp_server_running()){
	input_ready_wake_fd = mcp_wake_fd;
	input_ready_wake_run = mcp_ui_drain;
    }
    input_ready_timer_check = mcp_titlebar_spin_check;
    input_ready_timer_tick = mcp_titlebar_spin_tick;
}


/*
 * Run queued tools on the main thread.  A window-size signal must not
 * longjmp out of pith or c-client, so read_char's jump is disarmed for
 * the drain.  If a resize arrived meanwhile, take the jump now, which
 * is what the signal handler would have done.
 */
/*
 * The yes/no for an off-list mail_send.  Called from the command loop
 * (read_command_prep), where a question is allowed.  Default is no, and
 * ^C is no.  Afterwards the footer is redrawn.
 */
static int
mcp_ui_ask(const char *prompt)
{
    char q[300];
    int  ans;

    snprintf(q, sizeof(q), "%.280s", prompt);
    /* want_to turns an on-^C answer of 'n' into 0 and then only beeps,
     * so ^C answers 'x', which is not 'y' and so is no */
    ans = want_to(q, 'n', 'x', NO_HELP, WT_NORM);
    if(ps_global)
      ps_global->mangled_footer = 1;

    return(ans == 'y');
}


void
mcp_ui_confirm(void)
{
    if(mcp_enabled() && mcp_pending_confirms() > 0){
      mcp_titlebar_record_activity();
      mcp_confirm_pending(mcp_ui_ask);
    }
}


void
mcp_ui_drain(void)
{
#if	defined(SIGWINCH) && defined(TIOCGWINSZ)
    int armed = ready_for_winch;

    ready_for_winch = 0;
#endif

    if(mcp_drain() > 0)
      mcp_titlebar_record_activity();

#if	defined(SIGWINCH) && defined(TIOCGWINSZ)
    ready_for_winch = armed;
    if(armed && winch_occured)
      longjmp(winch_state, 1);
#endif
}


/*
 * MCP monitor screen, Setup / monItor (plan section 2.3).
 *
 * A scrolltool screen of plain text.  Each command key ends the screen,
 * the command runs here, and the screen is built again so the text
 * shows the new state.  Every change goes through mcp_config_save.
 */

static void mon_put(STORE_S *, char *, ...);
static void mon_text(STORE_S *);
static int  mon_cmd(int, MSGNO_S *, SCROLL_S *);
static int  mon_once(struct pine *);
static void mon_set_lock(int);
static void mon_toggle_aimon(void);
static void mon_edit_allow(void);
static void mon_drop_pending(void);


static void
mon_put(STORE_S *store, char *fmt, ...)
{
    char    buf[512];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    so_puts(store, buf);
}


static void
mon_text(STORE_S *store)
{
    char host[128], status[256], err[256], cerr[256], sock[256];
    int  port = 0, n;

    host[0] = status[0] = err[0] = cerr[0] = sock[0] = '\0';
    mcp_aimon_target(host, sizeof(host), &port);
    mcp_aimon_status(status, sizeof(status));
    mcp_server_last_error(err, sizeof(err));
    mcp_config_last_error(cerr, sizeof(cerr));
    mcp_socket_path(sock, sizeof(sock));
    n = mcp_allow_count();

    mon_put(store, "%s\n\n",
	    _("MCP monitor.  Settings are kept in $HOME/.config/alpine-mcp/config.json."));

    mon_put(store, "  %-22s %s\n", _("Operator lock:"),
	    mcp_lock_engaged()
	      ? _("ON   every tool call is refused with -32001")
	      : _("OFF  tool calls are served"));

    mon_put(store, "  %-22s %d %s\n", _("Recipient allow-list:"), n,
	    n == 1 ? _("entry") : _("entries"));

    if(mcp_server_running())
      mon_put(store, "  %-22s %s %s\n", _("Socket:"), _("open at"), sock);
    else
      mon_put(store, "  %-22s %s\n", _("Socket:"), _("not open"));

    mon_put(store, "  %-22s %s, %s:%d\n", _("aimon gateway:"),
	    mcp_aimon_enabled() ? _("enabled") : _("disabled"), host, port);
    mon_put(store, "  %-22s %s\n", _("aimon state:"), status);
    {
	char **pn = NULL;
	int    np = 0, k;

	(void) mcp_idem_pending(&pn, &np);
	mon_put(store, "  %-22s %d\n", _("Pending records:"), np);
	for(k = 0; k < np && k < 5; k++)
	  mon_put(store, "  %-22s %s\n", "", pn[k]);

	if(np > 5)
	  mon_put(store, "  %-22s %s\n", "", _("..."));

	for(k = 0; k < np; k++)
	  free(pn[k]);

	if(pn)
	  free(pn);
    }

    mon_put(store, "  %-22s %s\n", _("Last error:"),
	    err[0] ? err : cerr[0] ? cerr : _("none"));

    mon_put(store, "\n%s\n", _("Commands"));
    mon_put(store, "  (L) %s\n",
	    _("Lock       Refuse every tool call and write config.json."));
    mon_put(store, "  (U) %s\n",
	    _("Unlock     Serve tool calls again and write config.json."));
    mon_put(store, "  (W) %s\n",
	    _("Whitelist  Edit the recipients that may be mailed without asking."));
    mon_put(store, "      %s\n",
	    _("            One per line, local@domain or *@domain."));
    mon_put(store, "  (A) %s\n",
	    _("Aimon      Turn the aimon gateway connection on or off."));
    mon_put(store, "  (R) %s\n",
	    _("Reconnect  Close the aimon connection; it registers again at once."));
    mon_put(store, "  (P) %s\n",
	    _("Pending    Remove the first pending record listed above."));
    mon_put(store, "  (E) %s\n", _("Exit       Leave this screen."));
}


/* every monitor key ends the scroll screen; the choice is in data.i */
static int
mon_cmd(int cmd, MSGNO_S *msgmap, SCROLL_S *sparms)
{
    (void) msgmap;

    ps_global->next_screen = SCREEN_FUN_NULL;

    switch(cmd){
      case MC_MON_LOCK :
	sparms->proc.data.i = 'l';
	break;

      case MC_MON_UNLOCK :
	sparms->proc.data.i = 'u';
	break;

      case MC_MON_ALLOW :
	sparms->proc.data.i = 'w';
	break;

      case MC_MON_AIMON :
	sparms->proc.data.i = 'a';
	break;

      case MC_MON_RECONN :
	sparms->proc.data.i = 'r';
	break;

      case MC_MON_PENDING :
	sparms->proc.data.i = 'p';
	break;

      default :
	return(0);
    }

    return(1);
}


/* one pass of the screen; returns the command letter, or 'e' */
static int
mon_once(struct pine *ps)
{
    SCROLL_S sargs;
    STORE_S *store;
    int      cmd;

    if(!(store = so_get(CharStar, NULL, EDIT_ACCESS))){
	q_status_message(SM_ORDER | SM_DING, 3, 3, _("Error allocating space."));
	return('e');
    }

    mon_text(store);

    memset(&sargs, 0, sizeof(SCROLL_S));
    sargs.text.text   = so_text(store);
    sargs.text.src    = CharStar;
    sargs.text.desc   = _("MCP Monitor");
    sargs.bar.title   = cpystr(_("MCP MONITOR"));
    sargs.proc.tool   = mon_cmd;
    sargs.help.text   = NO_HELP;
    sargs.help.title  = NULL;
    sargs.keys.menu   = &mcp_monitor_keymenu;
    setbitmap(sargs.keys.bitmap);

    scrolltool(&sargs);

    ps->mangled_screen = 1;
    cmd = sargs.proc.data.i ? sargs.proc.data.i : 'e';

    so_give(&store);
    if(sargs.bar.title)
      fs_give((void **) &sargs.bar.title);

    return(cmd);
}


/*
 * Lock is fail-closed in both directions: engaging it stays engaged
 * even when config.json cannot be written; releasing it is undone when
 * the file cannot be written, so the file and the running state agree
 * about every unlock.
 */
static void
mon_set_lock(int on)
{
    char why[256];

    mcp_set_lock(on);
    if(mcp_config_save() < 0){
	mcp_config_last_error(why, sizeof(why));
	if(!on)
	  mcp_set_lock(1);

	q_status_message1(SM_ORDER | SM_DING, 3, 5,
			  on ? _("Lock engaged, but config.json was not written: %s")
			     : _("Still locked, config.json was not written: %s"),
			  (void *) why);
    }
    else
      q_status_message(SM_ORDER, 0, 3,
		       on ? _("Operator lock engaged")
			  : _("Operator lock released"));
}


static void
mon_toggle_aimon(void)
{
    char why[256];
    int  was = mcp_aimon_enabled();

    mcp_set_aimon_enabled(!was);
    if(mcp_config_save() < 0){
	mcp_config_last_error(why, sizeof(why));
	mcp_set_aimon_enabled(was);
	q_status_message1(SM_ORDER | SM_DING, 3, 5,
			  _("aimon setting unchanged, config.json was not written: %s"),
			  (void *) why);
	return;
    }

    mcp_aimon_kick(0);
    q_status_message(SM_ORDER, 0, 3,
		     was ? _("aimon gateway turned off")
			 : _("aimon gateway turned on"));
}


/* one address per line, edited in pico, applied only if every line is legal */
static void
mon_edit_allow(void)
{
    char **cur, **lst = NULL, *text = NULL, *result = NULL, *emsg;
    char  *p, *q, err[256];
    size_t len = 1;
    int    i, n = 0, off = 0;

    if((cur = mcp_allow_list_copy()) == NULL){
	q_status_message(SM_ORDER | SM_DING, 3, 5,
			 _("Cannot read the allow-list"));
	return;
    }

    for(i = 0; cur[i]; i++)
      len += strlen(cur[i]) + 1;

    text = (char *) fs_get(len);
    text[0] = '\0';
    for(i = 0; cur[i]; i++)
      off += snprintf(text + off, len - (size_t) off, "%s\n", cur[i]);

    emsg = signature_edit_lit(text, &result, _("MCP ALLOW-LIST"),
			      h_composer_sigedit);
    fs_give((void **) &text);
    if(emsg){
	q_status_message(SM_ORDER, 0, 3, emsg);
	fs_give((void **) &emsg);
	mcp_free_list(&cur);
	return;
    }

    /* split result in place; blank lines are dropped */
    for(p = result, i = 0; *p; p++)
      if(*p == '\n')
	i++;

    lst = (char **) fs_get((size_t) (i + 2) * sizeof(char *));
    for(p = result; p && *p; p = q){
	char *e;

	if((q = strchr(p, '\n')) != NULL)
	  *q++ = '\0';

	while(*p == ' ' || *p == '\t')
	  p++;

	e = p + strlen(p);
	while(e > p && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'))
	  *--e = '\0';

	if(*p)
	  lst[n++] = p;
    }

    lst[n] = NULL;

    if(mcp_set_allow_list(lst, err, sizeof(err)) < 0)
      q_status_message1(SM_ORDER | SM_DING, 3, 5,
			_("Allow-list unchanged: %s"), (void *) err);
    else if(mcp_config_save() < 0){
	mcp_config_last_error(err, sizeof(err));
	(void) mcp_set_allow_list(cur, NULL, 0);        /* put the old one back */
	q_status_message1(SM_ORDER | SM_DING, 3, 5,
			  _("Allow-list unchanged, config.json was not written: %s"),
			  (void *) err);
    }
    else{
	char msg[80];

	snprintf(msg, sizeof(msg), _("Allow-list saved: %d %s"),
		 mcp_allow_count(),
		 mcp_allow_count() == 1 ? _("entry") : _("entries"));
	q_status_message(SM_ORDER, 0, 3, msg);
    }

    fs_give((void **) &lst);
    fs_give((void **) &result);
    mcp_free_list(&cur);
}


/*
 * Remove the first pending idempotency record, after the human says yes.
 * A pending record means an earlier create/send never finished; removing
 * it lets a retry with that key run again, so the question names it.
 */
static void
mon_drop_pending(void)
{
    char **pn = NULL, q[300];
    int    np = 0, k;

    if(mcp_idem_pending(&pn, &np) < 0 || np == 0){
	q_status_message(SM_ORDER, 0, 3, _("No pending records"));
    }
    else{
	snprintf(q, sizeof(q), _("Remove pending record \"%.200s\""), pn[0]);
	if(want_to(q, 'n', 'n', NO_HELP, WT_NORM) == 'y'){
	    if(mcp_idem_delete_pending(pn[0]) == 0)
	      q_status_message(SM_ORDER, 0, 3, _("Pending record removed"));
	    else
	      q_status_message(SM_ORDER | SM_DING, 3, 3,
			       _("Could not remove the record"));
	}
    }

    for(k = 0; k < np; k++)
      free(pn[k]);

    if(pn)
      free(pn);
}


void
mcp_monitor_screen(struct pine *ps)
{
    int cmd;

    if(!mcp_enabled())
      return;

    for(;;){
	cmd = mon_once(ps);
	switch(cmd){
	  case 'l' :
	    mon_set_lock(1);
	    break;

	  case 'u' :
	    mon_set_lock(0);
	    break;

	  case 'w' :
	    mon_edit_allow();
	    break;

	  case 'a' :
	    mon_toggle_aimon();
	    mcp_titlebar_color_update();
	    break;

	  case 'r' :
	    if(mcp_aimon_enabled()){
		mcp_aimon_kick(1);
		mcp_titlebar_color_update();
		q_status_message(SM_ORDER, 0, 3,
				 _("aimon connection dropped, registering again"));
	    }
	    else
	      q_status_message(SM_ORDER, 0, 3,
			       _("aimon gateway is off; turn it on with A"));

	    break;

	  case 'p' :
	    mon_drop_pending();
	    break;

	  default :                     /* 'e' */
	    mcp_titlebar_color_update();
	    return;
	}
    }
}

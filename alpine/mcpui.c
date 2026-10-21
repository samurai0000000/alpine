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
#include "confscroll.h"
#include "conftype.h"

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
 * An interactive configuration and status screen built with conf_scroll_screen()
 * and CONF_S lines. Arrow keys navigate between fields; Enter/X toggles
 * checkboxes; Enter/C edits values; A adds recipients; D deletes recipients.
 * Direct keys (L, U, A, R, W, P, E) work across all rows.
 * Every change goes through mcp_config_save().
 */

typedef enum {
    MCP_LINE_INTRO = 0,
    MCP_LINE_LOCK_CHECKBOX,
    MCP_LINE_AIMON_CHECKBOX,
    MCP_LINE_AIMON_HOST,
    MCP_LINE_AIMON_PORT,
    MCP_LINE_ALLOW_HEADING,
    MCP_LINE_ALLOW_ITEM,
    MCP_LINE_STATUS_SOCKET,
    MCP_LINE_STATUS_AIMON,
    MCP_LINE_STATUS_PENDING,
    MCP_LINE_STATUS_ERROR
} MCP_LINE_TYPE_E;

#define MCP_LINE_ALLOW_ITEM_BASE 100

enum {
    MON_RET_STAY    = 0,
    MON_RET_REFRESH = 10,
    MON_RET_EXIT    = 20
};

static MCP_LINE_TYPE_E s_selected_type     = MCP_LINE_LOCK_CHECKBOX;
static int             s_selected_item_idx = -1;

static void mon_set_lock(int);
static void mon_toggle_aimon(void);
static void mon_edit_host(void);
static void mon_edit_port(void);
static void mon_add_allow_item(void);
static void mon_edit_allow_item(int);
static void mon_delete_allow_item(int);
static void mon_edit_allow(void);
static void mon_drop_pending(void);
static int  mcp_tool(struct pine *, int, CONF_S **, unsigned);
static void build_mcp_conflines(struct pine *, CONF_S **, CONF_S **,
                                MCP_LINE_TYPE_E, int);


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


static void
mon_edit_host(void)
{
    char host[128], err[256];
    char prompt[128];
    int port = 0;
    int flags = OE_APPEND_CURRENT | OE_DISALLOW_HELP;

    host[0] = '\0';
    mcp_aimon_target(host, sizeof(host), &port);
    snprintf(prompt, sizeof(prompt), "%s", _("Enter aimon host: "));

    if(optionally_enter(host, -(ps_global->ttyo ? FOOTER_ROWS(ps_global) : 3),
                        0, sizeof(host), prompt, NULL, NO_HELP, &flags) == 0){
        if(host[0] == '\0'){
            q_status_message(SM_ORDER | SM_DING, 3, 3, _("Host cannot be empty"));
            return;
        }
        if(mcp_set_aimon_target(host, port, err, sizeof(err)) < 0){
            q_status_message1(SM_ORDER | SM_DING, 3, 5,
                              _("Cannot set aimon host: %s"), (void *) err);
            return;
        }
        if(mcp_config_save() < 0){
            mcp_config_last_error(err, sizeof(err));
            q_status_message1(SM_ORDER | SM_DING, 3, 5,
                              _("Config was not written: %s"), (void *) err);
            return;
        }
        if(mcp_aimon_enabled()){
            mcp_aimon_kick(1);
        }
        q_status_message1(SM_ORDER, 0, 3,
                          _("aimon host set to %s"), (void *) host);
    }
}


static void
mon_edit_port(void)
{
    char host[128], buf[32], err[256];
    char prompt[128];
    int port = 0, new_port = 0;
    int flags = OE_APPEND_CURRENT | OE_DISALLOW_HELP;

    host[0] = buf[0] = '\0';
    mcp_aimon_target(host, sizeof(host), &port);
    snprintf(buf, sizeof(buf), "%d", port);
    snprintf(prompt, sizeof(prompt), "%s", _("Enter aimon port: "));

    if(optionally_enter(buf, -(ps_global->ttyo ? FOOTER_ROWS(ps_global) : 3),
                        0, sizeof(buf), prompt, NULL, NO_HELP, &flags) == 0){
        new_port = atoi(buf);
        if(new_port <= 0 || new_port > 65535){
            q_status_message(SM_ORDER | SM_DING, 3, 3,
                             _("Port must be between 1 and 65535"));
            return;
        }
        if(mcp_set_aimon_target(host, new_port, err, sizeof(err)) < 0){
            q_status_message1(SM_ORDER | SM_DING, 3, 5,
                              _("Cannot set aimon port: %s"), (void *) err);
            return;
        }
        if(mcp_config_save() < 0){
            mcp_config_last_error(err, sizeof(err));
            q_status_message1(SM_ORDER | SM_DING, 3, 5,
                              _("Config was not written: %s"), (void *) err);
            return;
        }
        if(mcp_aimon_enabled()){
            mcp_aimon_kick(1);
        }
        q_status_message1(SM_ORDER, 0, 3,
                          _("aimon port set to %s"), (void *) buf);
    }
}


static void
mon_add_allow_item(void)
{
    char addr[256], err[256], prompt[128];
    char **cur = NULL, **lst = NULL;
    int count = 0, i;
    int flags = OE_DISALLOW_HELP;

    addr[0] = '\0';
    snprintf(prompt, sizeof(prompt), "%s", _("Add allowed recipient: "));
    if(optionally_enter(addr, -(ps_global->ttyo ? FOOTER_ROWS(ps_global) : 3),
                        0, sizeof(addr), prompt, NULL, NO_HELP, &flags) != 0 || addr[0] == '\0'){
        return;
    }

    cur = mcp_allow_list_copy();
    if(cur){
        for(count = 0; cur[count]; count++)
            ;
    }

    lst = (char **) fs_get((size_t)(count + 2) * sizeof(char *));
    for(i = 0; i < count; i++){
        lst[i] = cpystr(cur[i]);
    }
    lst[count] = cpystr(addr);
    lst[count + 1] = NULL;

    if(mcp_set_allow_list(lst, err, sizeof(err)) < 0){
        q_status_message1(SM_ORDER | SM_DING, 3, 5,
                          _("Invalid recipient: %s"), (void *) err);
    }
    else if(mcp_config_save() < 0){
        mcp_config_last_error(err, sizeof(err));
        if(cur)
            (void) mcp_set_allow_list(cur, NULL, 0);
        q_status_message1(SM_ORDER | SM_DING, 3, 5,
                          _("Allow-list unchanged, config.json was not written: %s"),
                          (void *) err);
    }
    else {
        q_status_message1(SM_ORDER, 0, 3,
                          _("Added allowed recipient: %s"), (void *) addr);
    }

    for(i = 0; i < count + 1; i++){
        fs_give((void **) &lst[i]);
    }
    fs_give((void **) &lst);
    if(cur)
        mcp_free_list(&cur);
}


static void
mon_edit_allow_item(int idx)
{
    char addr[256], err[256], prompt[128];
    char **cur = NULL;
    int count = 0;
    int flags = OE_APPEND_CURRENT | OE_DISALLOW_HELP;

    cur = mcp_allow_list_copy();
    if(!cur)
        return;

    for(count = 0; cur[count]; count++)
        ;

    if(idx < 0 || idx >= count){
        mcp_free_list(&cur);
        return;
    }

    snprintf(addr, sizeof(addr), "%s", cur[idx]);
    snprintf(prompt, sizeof(prompt), "%s", _("Edit allowed recipient: "));
    if(optionally_enter(addr, -(ps_global->ttyo ? FOOTER_ROWS(ps_global) : 3),
                        0, sizeof(addr), prompt, NULL, NO_HELP, &flags) != 0 || addr[0] == '\0'){
        mcp_free_list(&cur);
        return;
    }

    fs_give((void **) &cur[idx]);
    cur[idx] = cpystr(addr);

    if(mcp_set_allow_list(cur, err, sizeof(err)) < 0){
        q_status_message1(SM_ORDER | SM_DING, 3, 5,
                          _("Invalid recipient: %s"), (void *) err);
    }
    else if(mcp_config_save() < 0){
        mcp_config_last_error(err, sizeof(err));
        q_status_message1(SM_ORDER | SM_DING, 3, 5,
                          _("Allow-list unchanged, config.json was not written: %s"),
                          (void *) err);
    }
    else {
        q_status_message1(SM_ORDER, 0, 3,
                          _("Updated allowed recipient: %s"), (void *) addr);
    }

    mcp_free_list(&cur);
}


static void
mon_delete_allow_item(int idx)
{
    char err[256];
    char **cur = NULL, **lst = NULL;
    int count = 0, i, n = 0;

    cur = mcp_allow_list_copy();
    if(!cur)
        return;

    for(count = 0; cur[count]; count++)
        ;

    if(idx < 0 || idx >= count){
        mcp_free_list(&cur);
        return;
    }

    lst = (char **) fs_get((size_t) count * sizeof(char *));
    for(i = 0; i < count; i++){
        if(i != idx){
            lst[n++] = cpystr(cur[i]);
        }
    }
    lst[n] = NULL;

    if(mcp_set_allow_list(lst, err, sizeof(err)) < 0){
        q_status_message1(SM_ORDER | SM_DING, 3, 5,
                          _("Cannot delete recipient: %s"), (void *) err);
    }
    else if(mcp_config_save() < 0){
        mcp_config_last_error(err, sizeof(err));
        (void) mcp_set_allow_list(cur, NULL, 0);
        q_status_message1(SM_ORDER | SM_DING, 3, 5,
                          _("Allow-list unchanged, config.json was not written: %s"),
                          (void *) err);
    }
    else {
        q_status_message(SM_ORDER, 0, 3, _("Recipient removed from allow-list"));
    }

    for(i = 0; i < n; i++){
        fs_give((void **) &lst[i]);
    }
    fs_give((void **) &lst);
    mcp_free_list(&cur);
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


static int
mcp_tool(struct pine *ps, int cmd, CONF_S **cl, unsigned int flags)
{
    long varmem;
    MCP_LINE_TYPE_E type = MCP_LINE_LOCK_CHECKBOX;
    int idx = -1;

    (void) ps;
    (void) flags;

    if(!cl || !*cl)
        return 0;

    varmem = (*cl)->varmem;
    if(varmem == MCP_LINE_LOCK_CHECKBOX){
        type = MCP_LINE_LOCK_CHECKBOX;
    } else if(varmem == MCP_LINE_AIMON_CHECKBOX){
        type = MCP_LINE_AIMON_CHECKBOX;
    } else if(varmem == MCP_LINE_AIMON_HOST){
        type = MCP_LINE_AIMON_HOST;
    } else if(varmem == MCP_LINE_AIMON_PORT){
        type = MCP_LINE_AIMON_PORT;
    } else if(varmem == MCP_LINE_ALLOW_HEADING){
        type = MCP_LINE_ALLOW_HEADING;
    } else if(varmem >= MCP_LINE_ALLOW_ITEM_BASE){
        type = MCP_LINE_ALLOW_ITEM;
        idx = (int)(varmem - MCP_LINE_ALLOW_ITEM_BASE - 1);
    }

    s_selected_type = type;
    s_selected_item_idx = idx;

    switch(cmd){
      case MC_EXIT:
        return MON_RET_EXIT;

      case MC_MON_LOCK:
        mon_set_lock(1);
        return MON_RET_REFRESH;

      case MC_MON_UNLOCK:
        mon_set_lock(0);
        return MON_RET_REFRESH;

      case MC_MON_RECONN:
        if(mcp_aimon_enabled()){
            mcp_aimon_kick(1);
            mcp_titlebar_color_update();
            q_status_message(SM_ORDER, 0, 3,
                             _("aimon connection dropped, registering again"));
        } else {
            q_status_message(SM_ORDER, 0, 3,
                             _("aimon gateway is off; turn it on with A"));
        }
        return MON_RET_REFRESH;

      case MC_MON_ALLOW:
        mon_edit_allow();
        return MON_RET_REFRESH;

      case MC_MON_PENDING:
        mon_drop_pending();
        return MON_RET_REFRESH;

      case MC_MON_AIMON:
        mon_toggle_aimon();
        mcp_titlebar_color_update();
        return MON_RET_REFRESH;

      case MC_ADD:
      case 'a':
      case 'A':
        if(type == MCP_LINE_ALLOW_ITEM || type == MCP_LINE_ALLOW_HEADING){
            mon_add_allow_item();
            s_selected_type = MCP_LINE_ALLOW_ITEM;
            s_selected_item_idx = mcp_allow_count() - 1;
        } else {
            mon_toggle_aimon();
            mcp_titlebar_color_update();
        }
        return MON_RET_REFRESH;

      case MC_DELETE:
      case 'd':
      case 'D':
        if(type == MCP_LINE_ALLOW_ITEM && idx >= 0){
            mon_delete_allow_item(idx);
            s_selected_type = MCP_LINE_ALLOW_ITEM;
            if(idx >= mcp_allow_count()){
                s_selected_item_idx = mcp_allow_count() - 1;
            }
        }
        return MON_RET_REFRESH;

      case MC_TOGGLE:
      case 'x':
      case 'X':
      case ctrl('M'):
      case ctrl('J'):
        if(type == MCP_LINE_LOCK_CHECKBOX){
            mon_set_lock(!mcp_lock_engaged());
            return MON_RET_REFRESH;
        } else if(type == MCP_LINE_AIMON_CHECKBOX){
            mon_toggle_aimon();
            mcp_titlebar_color_update();
            return MON_RET_REFRESH;
        } else if(type == MCP_LINE_AIMON_HOST){
            mon_edit_host();
            return MON_RET_REFRESH;
        } else if(type == MCP_LINE_AIMON_PORT){
            mon_edit_port();
            return MON_RET_REFRESH;
        } else if(type == MCP_LINE_ALLOW_ITEM){
            if(idx < 0){
                mon_add_allow_item();
                s_selected_type = MCP_LINE_ALLOW_ITEM;
                s_selected_item_idx = mcp_allow_count() - 1;
            } else {
                mon_edit_allow_item(idx);
            }
            return MON_RET_REFRESH;
        } else if(type == MCP_LINE_ALLOW_HEADING){
            mon_add_allow_item();
            s_selected_type = MCP_LINE_ALLOW_ITEM;
            s_selected_item_idx = mcp_allow_count() - 1;
            return MON_RET_REFRESH;
        }
        break;

      case MC_EDIT:
      case 'c':
      case 'C':
        if(type == MCP_LINE_AIMON_HOST){
            mon_edit_host();
            return MON_RET_REFRESH;
        } else if(type == MCP_LINE_AIMON_PORT){
            mon_edit_port();
            return MON_RET_REFRESH;
        } else if(type == MCP_LINE_ALLOW_ITEM){
            if(idx < 0){
                mon_add_allow_item();
                s_selected_type = MCP_LINE_ALLOW_ITEM;
                s_selected_item_idx = mcp_allow_count() - 1;
            } else {
                mon_edit_allow_item(idx);
            }
            return MON_RET_REFRESH;
        } else if(type == MCP_LINE_ALLOW_HEADING){
            mon_add_allow_item();
            s_selected_type = MCP_LINE_ALLOW_ITEM;
            s_selected_item_idx = mcp_allow_count() - 1;
            return MON_RET_REFRESH;
        } else if(type == MCP_LINE_LOCK_CHECKBOX){
            mon_set_lock(!mcp_lock_engaged());
            return MON_RET_REFRESH;
        } else if(type == MCP_LINE_AIMON_CHECKBOX){
            mon_toggle_aimon();
            mcp_titlebar_color_update();
            return MON_RET_REFRESH;
        }
        break;

      default:
        break;
    }

    return 0;
}


static void
build_mcp_conflines(struct pine *ps, CONF_S **first_line, CONF_S **start_line,
                    MCP_LINE_TYPE_E sel_type, int sel_idx)
{
    CONF_S *ctmp = NULL;
    char host[128], status[256], err[256], cerr[256], sock[256];
    char linebuf[512];
    char **allow_list = NULL;
    int port = 0, allow_count = 0, i;
    int np = 0;
    char **pn = NULL;

    (void) ps;

    host[0] = status[0] = err[0] = cerr[0] = sock[0] = '\0';
    mcp_aimon_target(host, sizeof(host), &port);
    mcp_aimon_status(status, sizeof(status));
    mcp_server_last_error(err, sizeof(err));
    mcp_config_last_error(cerr, sizeof(cerr));
    mcp_socket_path(sock, sizeof(sock));
    allow_count = mcp_allow_count();
    allow_list = mcp_allow_list_copy();
    (void) mcp_idem_pending(&pn, &np);

    *first_line = NULL;
    *start_line = NULL;

    /* Line 0: Intro text (CF_NOSELECT) */
    new_confline(&ctmp);
    *first_line = ctmp;
    ctmp->flags = CF_NOSELECT;
    ctmp->value = cpystr(_("MCP monitor.  Settings are kept in $HOME/.config/alpine-mcp/config.json."));
    ctmp->valoffset = 0;

    /* Blank line (CF_NOSELECT | CF_B_LINE) */
    new_confline(&ctmp);
    ctmp->flags = CF_NOSELECT | CF_B_LINE;

    /* Row 1: Operator lock checkbox (Selectable, flags = 0) */
    new_confline(&ctmp);
    ctmp->flags = 0;
    ctmp->valoffset = 0;
    ctmp->tool = mcp_tool;
    ctmp->keymenu = &mcp_config_keymenu;
    ctmp->varmem = MCP_LINE_LOCK_CHECKBOX;
    ctmp->help = NO_HELP;
    if(mcp_lock_engaged()){
        snprintf(linebuf, sizeof(linebuf), "[X]  %-22s %s", _("Operator lock:"),
                 _("ON   (refuses tool calls with -32001)"));
    } else {
        snprintf(linebuf, sizeof(linebuf), "[ ]  %-22s %s", _("Operator lock:"),
                 _("OFF  (tool calls are served)"));
    }
    ctmp->value = cpystr(linebuf);
    if(sel_type == MCP_LINE_LOCK_CHECKBOX)
        *start_line = ctmp;

    /* Row 2: aimon gateway enable checkbox (Selectable, flags = 0) */
    new_confline(&ctmp);
    ctmp->flags = 0;
    ctmp->valoffset = 0;
    ctmp->tool = mcp_tool;
    ctmp->keymenu = &mcp_config_keymenu;
    ctmp->varmem = MCP_LINE_AIMON_CHECKBOX;
    ctmp->help = NO_HELP;
    snprintf(linebuf, sizeof(linebuf), "[%c]  %-22s %s",
             mcp_aimon_enabled() ? 'X' : ' ',
             _("aimon gateway:"),
             mcp_aimon_enabled() ? _("enabled") : _("disabled"));
    ctmp->value = cpystr(linebuf);
    if(sel_type == MCP_LINE_AIMON_CHECKBOX)
        *start_line = ctmp;

    /* Row 3: aimon host (Selectable, flags = 0) */
    new_confline(&ctmp);
    ctmp->flags = 0;
    ctmp->valoffset = 0;
    ctmp->tool = mcp_tool;
    ctmp->keymenu = &mcp_config_keymenu;
    ctmp->varmem = MCP_LINE_AIMON_HOST;
    ctmp->help = NO_HELP;
    snprintf(linebuf, sizeof(linebuf), "     %-22s = %s", _("aimon host"), host);
    ctmp->value = cpystr(linebuf);
    if(sel_type == MCP_LINE_AIMON_HOST)
        *start_line = ctmp;

    /* Row 4: aimon port (Selectable, flags = 0) */
    new_confline(&ctmp);
    ctmp->flags = 0;
    ctmp->valoffset = 0;
    ctmp->tool = mcp_tool;
    ctmp->keymenu = &mcp_config_keymenu;
    ctmp->varmem = MCP_LINE_AIMON_PORT;
    ctmp->help = NO_HELP;
    snprintf(linebuf, sizeof(linebuf), "     %-22s = %d", _("aimon port"), port);
    ctmp->value = cpystr(linebuf);
    if(sel_type == MCP_LINE_AIMON_PORT)
        *start_line = ctmp;

    /* Row 5+: Allow-list heading and items */
    if(allow_count == 0){
        new_confline(&ctmp);
        ctmp->flags = 0;
        ctmp->valoffset = 0;
        ctmp->tool = mcp_tool;
        ctmp->keymenu = &mcp_config_keymenu;
        ctmp->varmem = MCP_LINE_ALLOW_ITEM_BASE;
        ctmp->help = NO_HELP;
        snprintf(linebuf, sizeof(linebuf), "     %-22s 0 entries", _("Recipient allow-list:"));
        ctmp->value = cpystr(linebuf);
        if(sel_type == MCP_LINE_ALLOW_ITEM || sel_type == MCP_LINE_ALLOW_HEADING)
            *start_line = ctmp;
    } else {
        new_confline(&ctmp);
        ctmp->flags = CF_NOSELECT;
        ctmp->valoffset = 0;
        snprintf(linebuf, sizeof(linebuf), "     %-22s %d %s", _("Recipient allow-list:"),
                 allow_count, allow_count == 1 ? _("entry") : _("entries"));
        ctmp->value = cpystr(linebuf);

        for(i = 0; i < allow_count; i++){
            new_confline(&ctmp);
            ctmp->flags = 0;
            ctmp->valoffset = 0;
            ctmp->tool = mcp_tool;
            ctmp->keymenu = &mcp_config_keymenu;
            ctmp->varmem = MCP_LINE_ALLOW_ITEM_BASE + 1 + i;
            ctmp->help = NO_HELP;
            snprintf(linebuf, sizeof(linebuf), "       - %s",
                     allow_list && allow_list[i] ? allow_list[i] : "");
            ctmp->value = cpystr(linebuf);
            if(sel_type == MCP_LINE_ALLOW_ITEM && sel_idx == i)
                *start_line = ctmp;
        }
        if((sel_type == MCP_LINE_ALLOW_ITEM || sel_type == MCP_LINE_ALLOW_HEADING) && *start_line == NULL){
            *start_line = ctmp;
        }
    }

    /* Blank separator line before Telemetry (CF_NOSELECT | CF_B_LINE) */
    new_confline(&ctmp);
    ctmp->flags = CF_NOSELECT | CF_B_LINE;

    /* Socket status (CF_NOSELECT) */
    new_confline(&ctmp);
    ctmp->flags = CF_NOSELECT;
    ctmp->valoffset = 0;
    if(mcp_server_running()){
        snprintf(linebuf, sizeof(linebuf), "  %-22s %s %s", _("Socket:"), _("open at"), sock);
    } else {
        snprintf(linebuf, sizeof(linebuf), "  %-22s %s", _("Socket:"), _("not open"));
    }
    ctmp->value = cpystr(linebuf);

    /* aimon state (CF_NOSELECT) */
    new_confline(&ctmp);
    ctmp->flags = CF_NOSELECT;
    ctmp->valoffset = 0;
    snprintf(linebuf, sizeof(linebuf), "  %-22s %s", _("aimon state:"), status);
    ctmp->value = cpystr(linebuf);

    /* Pending records (CF_NOSELECT) */
    new_confline(&ctmp);
    ctmp->flags = CF_NOSELECT;
    ctmp->valoffset = 0;
    snprintf(linebuf, sizeof(linebuf), "  %-22s %d", _("Pending records:"), np);
    ctmp->value = cpystr(linebuf);

    for(i = 0; i < np && i < 5; i++){
        new_confline(&ctmp);
        ctmp->flags = CF_NOSELECT;
        ctmp->valoffset = 0;
        snprintf(linebuf, sizeof(linebuf), "  %-22s %s", "", pn[i]);
        ctmp->value = cpystr(linebuf);
    }
    if(np > 5){
        new_confline(&ctmp);
        ctmp->flags = CF_NOSELECT;
        ctmp->valoffset = 0;
        snprintf(linebuf, sizeof(linebuf), "  %-22s %s", "", _("..."));
        ctmp->value = cpystr(linebuf);
    }

    /* Last error (CF_NOSELECT) */
    new_confline(&ctmp);
    ctmp->flags = CF_NOSELECT;
    ctmp->valoffset = 0;
    snprintf(linebuf, sizeof(linebuf), "  %-22s %s", _("Last error:"),
             err[0] ? err : cerr[0] ? cerr : _("none"));
    ctmp->value = cpystr(linebuf);

    /* Clean up temporary data */
    if(allow_list)
        mcp_free_list(&allow_list);

    for(i = 0; i < np; i++)
        free(pn[i]);
    if(pn)
        free(pn);

    if(*start_line == NULL){
        *start_line = first_sel_confline(*first_line);
    }
}


void
mcp_monitor_screen(struct pine *ps)
{
    if(!mcp_enabled())
      return;

    s_selected_type = MCP_LINE_LOCK_CHECKBOX;
    s_selected_item_idx = -1;

    for(;;){
        CONF_S *first_line = NULL;
        CONF_S *start_line = NULL;
        OPT_SCREEN_S screen;

        memset(&screen, 0, sizeof(screen));
        build_mcp_conflines(ps, &first_line, &start_line, s_selected_type, s_selected_item_idx);

        if(BODY_LINES(ps) < 1){
            free_conflines(&first_line);
            break;
        }

        int rv = conf_scroll_screen(ps, &screen, start_line, _("MCP MONITOR"), _("MCP Monitor"), 0, NULL);
        if(rv == MON_RET_EXIT || rv <= 3){
            break;
        }
    }

    mcp_titlebar_color_update();
    ps->mangled_screen = 1;
}

/* $OpenBSD$ */

/*
 * Experimental tmux control-mode mirror.
 *
 * This file makes a local tmux server act as a control-mode client to a
 * remote "tmux -C attach" stream.  It is deliberately a prototype: it starts
 * a child process (normally ssh, but for testing just another local tmux),
 * parses the control protocol and creates local windows and panes which are
 * kept in step with the remote session.
 *
 * The implementation is based on ideas from Sergei Nizovtsev's draft remote
 * session support, but uses the current next-3.9 APIs instead of its older
 * client_window/client_windows structures.
 */

#include <sys/types.h>
#include <sys/wait.h>

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tmux.h"

/* Pane ID mapping. */
struct remote_pane {
	u_int			 id;		/* remote pane id (%N) */
	u_int			 window_id;	/* remote window id (@N) */
	u_int			 idx;		/* remote pane index */
	int			 active;
	int			 seen;

	struct window_pane	*wp;
	struct bufferevent	*event;		/* our end of the pair */

	struct remote		*r;
	RB_ENTRY(remote_pane)	 entry;
};
RB_HEAD(remote_panes, remote_pane);

/* Window ID mapping. */
struct remote_window {
	u_int			 id;		/* remote window id (@N) */
	u_int			 idx;		/* remote window index */
	int			 active;
	int			 seen;
	int			 want_active;

	struct window		*w;
	struct winlink		*wl;

	char			*layout;
	char			*name;

	RB_ENTRY(remote_window)	 entry;
};
RB_HEAD(remote_windows, remote_window);

struct remote_query {
	char			*command;
	void			 (*done)(struct remote *, struct remote_query *);
	void			 (*error)(struct remote *, struct remote_query *);
	u_int			 window_id;

	TAILQ_ENTRY(remote_query) entry;
};

struct remote_early {
	u_int			 id;
	struct evbuffer		*data;
	TAILQ_ENTRY(remote_early) entry;
};

struct remote {
	u_int			 id;
	struct job		*job;
	struct bufferevent	*event;

	struct evbuffer		*reply;
	struct remote_windows	 windows;
	struct remote_panes	 panes;
	TAILQ_HEAD(, remote_query) queries;
	TAILQ_HEAD(, remote_early) early;

	u_int			 reply_number;
	long			 reply_time;
	u_int			 reply_flags;

	struct session		*session;
	char			*session_name;
	char			*remote_name;
	char			*host;
	char			*port;
	char			*token;
	int			 name_locked;
	u_int			 session_id;
	int			 booting;
	int			 booted;

	struct cmdq_item	*wait_item;
	struct client		*wait_client;
	int			 wait_inside;
	int			 wait_detached;

	TAILQ_ENTRY(remote)	 entry;
};
static TAILQ_HEAD(remotes, remote) remotes = TAILQ_HEAD_INITIALIZER(remotes);
static u_int next_remote_id;

static int	remote_window_cmp(struct remote_window *,
		    struct remote_window *);
static int	remote_pane_cmp(struct remote_pane *, struct remote_pane *);
RB_PROTOTYPE_STATIC(remote_windows, remote_window, entry, remote_window_cmp);
RB_PROTOTYPE_STATIC(remote_panes, remote_pane, entry, remote_pane_cmp);

static void	remote_free(struct remote *);
static void	remote_reset(struct remote *);
static void	remote_read_line(struct remote *, char *);
static void	remote_dispatch_event(struct remote *, char *);
static void	remote_reply_begin(struct remote *, char *);
static void	remote_reply_end(struct remote *, char *, int);
static struct remote_query *remote_run(struct remote *,
		    struct remote_query *, const char *, ...)
		    printflike(3, 4);
static struct remote_query *remote_query_new(
		    void (*)(struct remote *, struct remote_query *),
		    void (*)(struct remote *, struct remote_query *));
static void	remote_query_free(struct remote_query *);

static void	remote_bootstrap_refresh(struct remote *,
		    struct remote_query *);
static void	remote_bootstrap_windows(struct remote *,
		    struct remote_query *);
static void	remote_bootstrap_panes(struct remote *,
		    struct remote_query *);
static void	remote_bootstrap_error(struct remote *,
		    struct remote_query *);
static void	remote_job_update(struct job *);
static void	remote_job_complete(struct job *);

static void	remote_window_add(struct remote *, u_int);
static void	remote_window_add_done(struct remote *, struct remote_query *);
static void	remote_window_panes_done(struct remote *,
		    struct remote_query *);
static void	remote_window_close(struct remote *, u_int);
static void	remote_window_renamed(struct remote *, u_int, char *);
static void	remote_window_pane_changed(struct remote *, u_int, u_int);
static void	remote_session_changed(struct remote *, u_int, char *);
static void	remote_session_window_changed(struct remote *, u_int, u_int);
static void	remote_layout_change(struct remote *, u_int, char *);
static void	remote_output(struct remote *, u_int, char *);
static void	remote_exit(struct remote *);

static void	remote_pane_input(struct bufferevent *, void *);
static void	remote_flush_early(struct remote *, u_int);

static struct remote_window *remote_window_get(struct remote *, u_int, int);
static struct remote_pane *remote_pane_get(struct remote *, u_int, int);
static void	remote_create_session(struct remote *);
static void	remote_destroy_session(struct remote *);
static void	remote_create_local_window(struct remote *, struct remote_window *);
static void	remote_parse_window_line(struct remote *, char *);
static void	remote_parse_pane_line(struct remote *, char *);
static void	remote_sync_layout(struct remote *, struct remote_window *);
static void	remote_prune_panes(struct remote *, struct remote_window *);
static void	remote_reorder_panes(struct remote *, struct remote_window *);
static void	remote_set_active_window(struct remote *);
static void	remote_layout_size(const char *, u_int *, u_int *);

static void
remote_log(struct remote *r, const char *fmt, ...)
{
	va_list	ap;
	char	*msg;

	va_start(ap, fmt);
	xvasprintf(&msg, fmt, ap);
	va_end(ap);
	log_debug("remote #%u: %s", r->id, msg);
	free(msg);
}

/* Compare remote windows by id. */
static int
remote_window_cmp(struct remote_window *rw1, struct remote_window *rw2)
{
	if (rw1->id < rw2->id)
		return (-1);
	if (rw1->id > rw2->id)
		return (1);
	return (0);
}
RB_GENERATE_STATIC(remote_windows, remote_window, entry, remote_window_cmp);

/* Compare remote panes by id. */
static int
remote_pane_cmp(struct remote_pane *rp1, struct remote_pane *rp2)
{
	if (rp1->id < rp2->id)
		return (-1);
	if (rp1->id > rp2->id)
		return (1);
	return (0);
}
RB_GENERATE_STATIC(remote_panes, remote_pane, entry, remote_pane_cmp);

/* Allocate a query. */
static struct remote_query *
remote_query_new(void (*done)(struct remote *, struct remote_query *),
    void (*error)(struct remote *, struct remote_query *))
{
	struct remote_query	*q;

	q = xcalloc(1, sizeof *q);
	q->done = done;
	q->error = error;
	return (q);
}

static void
remote_query_free(struct remote_query *q)
{
	free(q->command);
	free(q);
}

/* Send a command to the remote control client. */
static struct remote_query *
remote_run(struct remote *r, struct remote_query *q, const char *fmt, ...)
{
	va_list	ap;
	char	*line;

	va_start(ap, fmt);
	xvasprintf(&line, fmt, ap);
	va_end(ap);

	if (r->event == NULL) {
		free(line);
		if (q != NULL)
			remote_query_free(q);
		return (NULL);
	}
	log_debug("remote #%u command: %s", r->id, line);
	bufferevent_write(r->event, line, strlen(line));
	bufferevent_write(r->event, "\n", 1);

	if (q != NULL) {
		q->command = line;
		TAILQ_INSERT_TAIL(&r->queries, q, entry);
	} else
		free(line);
	return (q);
}

/* Get a window mapping. */
static struct remote_window *
remote_window_get(struct remote *r, u_int id, int create)
{
	struct remote_window	 key = { .id = id };
	struct remote_window	*rw;

	rw = RB_FIND(remote_windows, &r->windows, &key);
	if (rw == NULL && create) {
		rw = xcalloc(1, sizeof *rw);
		rw->id = id;
		RB_INSERT(remote_windows, &r->windows, rw);
	}
	return (rw);
}

/* Get a pane mapping. */
static struct remote_pane *
remote_pane_get(struct remote *r, u_int id, int create)
{
	struct remote_pane	 key = { .id = id };
	struct remote_pane	*rp;

	rp = RB_FIND(remote_panes, &r->panes, &key);
	if (rp == NULL && create) {
		rp = xcalloc(1, sizeof *rp);
		rp->id = id;
		rp->r = r;
		RB_INSERT(remote_panes, &r->panes, rp);
	}
	return (rp);
}

/* Buffer output for a pane that does not exist yet. */
static void
remote_buffer_early(struct remote *r, u_int id, const u_char *data, size_t len)
{
	struct remote_early	*re;

	TAILQ_FOREACH(re, &r->early, entry) {
		if (re->id == id)
			break;
	}
	if (re == NULL) {
		re = xcalloc(1, sizeof *re);
		re->id = id;
		re->data = evbuffer_new();
		TAILQ_INSERT_TAIL(&r->early, re, entry);
	}
	evbuffer_add(re->data, data, len);
}

/* Flush buffered output once a pane is created. */
static void
remote_flush_early(struct remote *r, u_int id)
{
	struct remote_early	*re, *re1;
	struct remote_pane	*rp;
	size_t			 len;
	u_char			*data;

	TAILQ_FOREACH_SAFE(re, &r->early, entry, re1) {
		if (re->id != id)
			continue;
		rp = remote_pane_get(r, id, 0);
		if (rp != NULL && rp->wp != NULL) {
			len = EVBUFFER_LENGTH(re->data);
			data = EVBUFFER_DATA(re->data);
			if (len != 0)
				input_parse_buffer(rp->wp, data, len);
		}
		TAILQ_REMOVE(&r->early, re, entry);
		evbuffer_free(re->data);
		free(re);
	}
}

/* Handle output from a remote pane. */
static void
remote_output(struct remote *r, u_int pane_id, char *data)
{
	struct remote_pane	*rp = remote_pane_get(r, pane_id, 0);
	char			*out;
	int			 len;

	if (*data == '\0')
		return;

	out = xmalloc(strlen(data) + 1);
	len = strunvis(out, data);
	if (len < 0) {
		remote_log(r, "bad escaped output for %%%u", pane_id);
		free(out);
		return;
	}

	if (rp == NULL || rp->wp == NULL)
		remote_buffer_early(r, pane_id, out, len);
	else
		input_parse_buffer(rp->wp, out, len);
	free(out);
}

/* Input from a local pane: send it to the remote as literal hex keys. */
static void
remote_pane_input(struct bufferevent *bev, void *data)
{
	struct remote_pane	*rp = data;
	struct evbuffer		*in = bev->input;
	size_t			 i, n = EVBUFFER_LENGTH(in);
	const u_char		*bytes;
	char			*hex;

	if (n == 0)
		return;
	bytes = EVBUFFER_DATA(in);

	hex = xmalloc(n * 3 + 1);
	for (i = 0; i < n; i++)
		xsnprintf(hex + i * 3, 4, "%02X ", bytes[i]);
	hex[n * 3] = '\0';

	log_debug("remote #%u: input %zu bytes for %%%u", rp->r->id, n,
	    rp->id);
	remote_run(rp->r, NULL, "send-keys -t %%%u -lH %s", rp->id, hex);
	free(hex);

	evbuffer_drain(in, n);
}

/* Create the local session mirroring the remote session. */
static void
remote_create_session(struct remote *r)
{
	char			*name, *base;
	struct environ		*env;
	struct options		*oo;
	u_int			 n = 1;

	if (r->session != NULL)
		return;
	if (r->session_name == NULL)
		base = xstrdup("remote");
	else
		base = xstrdup(r->session_name);

	name = xstrdup(base);
	while (session_find(name) != NULL) {
		free(name);
		xasprintf(&name, "%s-%u", base, n++);
	}

	env = environ_create();
	oo = options_create(global_s_options);
	r->session = session_create(NULL, name, "/tmp", env, oo, NULL);
	if (r->host != NULL)
		options_set_string(r->session->options, "@remote_host", 0,
		    "%s", r->host);
	if (r->remote_name != NULL)
		options_set_string(r->session->options, "@remote_session", 0,
		    "%s", r->remote_name);
	if (r->port != NULL)
		options_set_string(r->session->options, "@remote_port", 0,
		    "%s", r->port);
	if (r->token != NULL)
		options_set_string(r->session->options, "@remote_token", 0,
		    "%s", r->token);
	log_debug("remote #%u: created session %s", r->id, name);

	free(name);
	free(base);
}

/* Destroy the local mirror session and all of its windows. */
static void
remote_destroy_session(struct remote *r)
{
	struct session		*s = r->session;
	struct remote_window	*rw;

	r->session = NULL;

	while ((rw = RB_MIN(remote_windows, &r->windows)) != NULL) {
		RB_REMOVE(remote_windows, &r->windows, rw);
		free(rw->layout);
		free(rw->name);
		free(rw);
	}

	if (s != NULL) {
		if (s->curw != NULL)
			session_destroy(s, 1, __func__);
		/* A session with no windows cannot currently be destroyed. */
	}
}

/* Create a local window for a remote window. */
static void
remote_create_local_window(struct remote *r, struct remote_window *rw)
{
	struct window	*w;
	struct winlink	*wl;
	u_int		 sx, sy;

	remote_layout_size(rw->layout, &sx, &sy);
	w = window_create(sx, sy, 0, 0);

	/*
	 * Do not use session_attach(): it fires window-linked while the
	 * session may not have a current window yet, and hook expansion then
	 * dereferences curw.  Mirror sessions do not need those events.
	 */
	if (winlink_find_by_index(&r->session->windows, rw->idx) != NULL)
		wl = winlink_add(&r->session->windows, -1);
	else
		wl = winlink_add(&r->session->windows, rw->idx);
	if (wl == NULL) {
		remote_log(r, "cannot add winlink");
		window_remove_ref(w, __func__);
		return;
	}
	wl->session = r->session;
	if (r->session->curw == NULL)
		r->session->curw = wl;
	winlink_set_window(wl, w);

	rw->w = w;
	rw->wl = wl;
	options_set_number(w->options, "automatic-rename", 0);
	if (rw->name != NULL)
		window_set_name(w, rw->name, 0);
	log_debug("remote #%u: created window @%u as @%u index %d", r->id,
	    rw->id, w->id, wl->idx);
}

/* Create a local pane and input pair for a remote pane. */
static struct remote_pane *
remote_create_local_pane(struct remote *r, struct remote_window *rw,
    u_int id, u_int idx, u_int cx, u_int cy, u_int hlimit, u_int active)
{
	struct remote_pane	*rp;
	struct window_pane	*wp, *other;
	struct bufferevent	*pair[2];

	if (rw->w == NULL)
		return (NULL);
	rp = remote_pane_get(r, id, 0);
	if (rp != NULL)
		return (rp);

	rp = remote_pane_get(r, id, 1);
	rp->window_id = rw->id;
	rp->idx = idx;
	rp->active = active;
	rp->seen = 1;

	other = rw->w->active;
	if (other == NULL)
		other = TAILQ_LAST(&rw->w->panes, window_panes);
	wp = window_add_pane(rw->w, other, hlimit == 0 ? 2000 : hlimit, 0);
	if (wp == NULL) {
		RB_REMOVE(remote_panes, &r->panes, rp);
		free(rp);
		return (NULL);
	}
	if (rw->w->layout_root == NULL)
		layout_init(rw->w, wp);
	if (rw->w->active == NULL)
		rw->w->active = wp;
	wp->base.cx = cx;
	wp->base.cy = cy;

	if (bufferevent_pair_new(NULL, 0, pair) != 0) {
		remote_log(r, "cannot create pane event pair");
		RB_REMOVE(remote_panes, &r->panes, rp);
		free(rp);
		server_kill_pane(wp);
		return (NULL);
	}
	wp->event = pair[1];
	wp->ictx = input_init(wp, &wp->palette);
	wp->remote = r;
	wp->remote_pane = rp;
	wp->flags |= PANE_REMOTE;
	if (r->host != NULL)
		options_set_string(wp->options, "@remote_host", 0, "%s",
		    r->host);
	if (r->remote_name != NULL)
		options_set_string(wp->options, "@remote_session", 0, "%s",
		    r->remote_name);

	rp->wp = wp;
	rp->event = pair[0];
	bufferevent_setcb(pair[0], remote_pane_input, NULL, NULL, rp);
	bufferevent_enable(pair[0], EV_READ);
	bufferevent_enable(pair[1], EV_READ|EV_WRITE);

	log_debug("remote #%u: created pane %%%u as %%%u in @%u", r->id,
	    id, wp->id, rw->w->id);
	return (rp);
}


/* Find the mirror for a local session. */
struct remote *
remote_find_by_session(struct session *s)
{
	struct remote	*r;

	TAILQ_FOREACH(r, &remotes, entry) {
		if (r->session == s)
			return (r);
	}
	return (NULL);
}


/* Find the remote window id for a local mirrored window. */
static int
remote_window_id_for(struct remote *r, struct window *w, u_int *id)
{
	struct remote_window	*rw;

	RB_FOREACH(rw, remote_windows, &r->windows) {
		if (rw->w == w) {
			*id = rw->id;
			return (0);
		}
	}
	return (-1);
}

/* Find the remote pane id for a local mirrored pane. */
static int
remote_pane_id_for(struct remote *r, struct window_pane *wp, u_int *id)
{
	struct remote_pane	*rp;

	RB_FOREACH(rp, remote_panes, &r->panes) {
		if (rp->wp == wp) {
			*id = rp->id;
			return (0);
		}
	}
	return (-1);
}

/* Commands that operate on the remote window/pane model. */
static int
remote_command_is_forwarded(const char *name)
{
	static const char *names[] = {
		"break-pane", "clear-history", "clock-mode", "copy-mode",
		"display-panes", "find-window", "join-pane", "kill-pane",
		"kill-window", "last-pane", "last-window", "link-window",
		"move-pane", "move-window", "new-pane", "new-window",
		"next-layout", "next-window", "previous-layout",
		"previous-window", "rename-window", "resize-pane",
		"resize-window", "rotate-window", "select-layout",
		"select-pane", "select-window", "split-window",
		"swap-pane", "swap-window", "switch-mode", "unlink-window",
		NULL
	};
	int	i;

	for (i = 0; names[i] != NULL; i++) {
		if (strcmp(name, names[i]) == 0)
			return (1);
	}
	return (0);
}

/*
 * Route a local command to the remote control stream. Returns 1 if handled,
 * -1 on error, 0 if the command should run locally.
 */
int
remote_route_command(struct cmdq_item *item, struct cmd *cmd,
    const struct cmd_entry *entry)
{
	struct args	*args = cmd_get_args(cmd);
	struct remote	*r;
	char		*line;

	if (!remote_command_is_forwarded(entry->name))
		return (0);
	r = remote_find_by_session(cmdq_get_target(item)->s);
	if (r == NULL)
		return (0);
	if (args_has(args, 't')) {
		struct cmd_find_state	*target = cmdq_get_target(item);
		u_int			 id;

		if ((strcmp(entry->name, "new-window") == 0 &&
		    target->s == r->session)) {
			xasprintf(&line, "new-window -t $%u", r->session_id);
		} else if (strcmp(entry->name, "split-window") == 0 &&
		    target->w != NULL &&
		    remote_window_id_for(r, target->w, &id) == 0) {
			xasprintf(&line, "split-window -t @%u", id);
		} else if ((strcmp(entry->name, "select-window") == 0 ||
		    strcmp(entry->name, "kill-window") == 0 ||
		    strcmp(entry->name, "rename-window") == 0 ||
		    strcmp(entry->name, "resize-window") == 0) &&
		    target->w != NULL &&
		    remote_window_id_for(r, target->w, &id) == 0) {
			xasprintf(&line, "%s -t @%u", entry->name, id);
		} else if ((strcmp(entry->name, "select-pane") == 0 ||
		    strcmp(entry->name, "kill-pane") == 0 ||
		    strcmp(entry->name, "resize-pane") == 0) &&
		    target->wp != NULL &&
		    remote_pane_id_for(r, target->wp, &id) == 0) {
			xasprintf(&line, "%s -t %%%u", entry->name, id);
		} else {
			cmdq_error(item, "targeted remote command not "
			    "supported yet");
			return (-1);
		}
	} else
		line = cmd_print(cmd);
	log_debug("remote #%u: command: %s", r->id, line);
	remote_run(r, NULL, "%s", line);
	free(line);
	return (1);
}

/* Free a pane's remote side. Called from window_pane_destroy(). */
void
remote_pane_destroy(struct window_pane *wp)
{
	struct remote_pane	*rp = wp->remote_pane;
	struct remote		*r;

	if (rp == NULL)
		return;
	r = rp->r;
	RB_REMOVE(remote_panes, &r->panes, rp);
	if (rp->event != NULL)
		bufferevent_free(rp->event);
	rp->event = NULL;
	wp->remote_pane = NULL;
	wp->remote = NULL;
	free(rp);
}

/* Work out a window size from a layout string. */
static void
remote_layout_size(const char *layout, u_int *sx, u_int *sy)
{
	const char	*p;

	*sx = 80;
	*sy = 24;
	if (layout == NULL)
		return;
	if ((p = strstr(layout, "\"w\":")) != NULL) {
		if (sscanf(p, "\"w\":%u,\"h\":%u", sx, sy) == 2)
			return;
	}
	if (sscanf(layout, "%*[^,],%ux%u", sx, sy) == 2)
		return;
	*sx = 80;
	*sy = 24;
}

/* Parse a tab-separated list-windows line. */
static void
remote_parse_window_line(struct remote *r, char *line)
{
	char			*iter = line, *field;
	struct remote_window	*rw;
	u_int			 id, idx, active;
	char			*name, *layout;

	field = strsep(&iter, "\t");
	if (field == NULL || field[0] != '@')
		return;
	id = strtoul(field + 1, NULL, 10);
	field = strsep(&iter, "\t");
	if (field == NULL)
		return;
	idx = strtoul(field, NULL, 10);
	name = strsep(&iter, "\t");
	if (name == NULL)
		return;
	layout = strsep(&iter, "\t");
	if (layout == NULL)
		return;
	field = strsep(&iter, "\t");
	active = field != NULL ? strtoul(field, NULL, 10) : 0;

	rw = remote_window_get(r, id, 1);
	rw->idx = idx;
	rw->active = active;
	free(rw->layout);
	rw->layout = xstrdup(layout);
	free(rw->name);
	rw->name = xstrdup(name);

	if (rw->w == NULL && r->session != NULL)
		remote_create_local_window(r, rw);
	else if (rw->w != NULL) {
		if (rw->name != NULL)
			window_set_name(rw->w, rw->name, 0);
	}
}

/* Parse a tab-separated list-panes line. */
static void
remote_parse_pane_line(struct remote *r, char *line)
{
	char			*iter = line, *field;
	struct remote_window	*rw;
	struct remote_pane	*rp;
	u_int			 wid, id, idx, active, cx, cy, hlimit;

	field = strsep(&iter, "\t");
	if (field == NULL || field[0] != '@')
		return;
	wid = strtoul(field + 1, NULL, 10);
	field = strsep(&iter, "\t");
	if (field == NULL || field[0] != '%')
		return;
	id = strtoul(field + 1, NULL, 10);
	field = strsep(&iter, "\t");
	if (field == NULL)
		return;
	idx = strtoul(field, NULL, 10);
	field = strsep(&iter, "\t");
	active = field != NULL ? strtoul(field, NULL, 10) : 0;
	field = strsep(&iter, "\t");
	cx = field != NULL ? strtoul(field, NULL, 10) : 0;
	field = strsep(&iter, "\t");
	cy = field != NULL ? strtoul(field, NULL, 10) : 0;
	field = strsep(&iter, "\t");
	hlimit = field != NULL ? strtoul(field, NULL, 10) : 2000;

	rw = remote_window_get(r, wid, 0);
	if (rw == NULL || rw->w == NULL)
		return;
	rw->seen = 1;

	rp = remote_pane_get(r, id, 0);
	if (rp == NULL) {
		rp = remote_create_local_pane(r, rw, id, idx, cx, cy, hlimit,
		    active);
		if (rp == NULL)
			return;
	} else {
		rp->seen = 1;
		rp->idx = idx;
		rp->active = active;
		rp->window_id = wid;
		if (rp->wp != NULL) {
			rp->wp->base.cx = cx;
			rp->wp->base.cy = cy;
		}
	}
	remote_flush_early(r, id);
}

/* Compare panes by remote index for reordering. */
static int
remote_pane_idx_cmp(const void *a, const void *b)
{
	const struct remote_pane *rpa = *(const struct remote_pane **)a;
	const struct remote_pane *rpb = *(const struct remote_pane **)b;

	if (rpa->idx < rpb->idx)
		return (-1);
	if (rpa->idx > rpb->idx)
		return (1);
	return (0);
}

/* Put local panes into remote pane index order. */
static void
remote_reorder_panes(struct remote *r, struct remote_window *rw)
{
	struct remote_pane	*rp, **array;
	struct window_pane	*wp;
	u_int			 n = 0, i = 0;

	RB_FOREACH(rp, remote_panes, &r->panes) {
		if (rp->window_id == rw->id && rp->wp != NULL)
			n++;
	}
	if (n < 2)
		return;
	array = xcalloc(n, sizeof *array);
	RB_FOREACH(rp, remote_panes, &r->panes) {
		if (rp->window_id == rw->id && rp->wp != NULL)
			array[i++] = rp;
	}
	qsort(array, n, sizeof *array, remote_pane_idx_cmp);

	TAILQ_FOREACH(wp, &rw->w->panes, entry)
		TAILQ_REMOVE(&rw->w->panes, wp, entry);
	TAILQ_FOREACH(wp, &rw->w->z_index, zentry)
		TAILQ_REMOVE(&rw->w->z_index, wp, zentry);
	for (i = 0; i < n; i++) {
		TAILQ_INSERT_TAIL(&rw->w->panes, array[i]->wp, entry);
		TAILQ_INSERT_TAIL(&rw->w->z_index, array[i]->wp, zentry);
	}
	free(array);
}

/* Remove local panes which are no longer present remotely. */
static void
remote_prune_panes(struct remote *r, struct remote_window *rw)
{
	struct remote_pane	*rp, *rp1;

	if (rw->w == NULL)
		return;
	if (window_count_panes(rw->w, 1) <= 1)
		return;
	RB_FOREACH_SAFE(rp, remote_panes, &r->panes, rp1) {
		if (rp->window_id != rw->id || rp->seen)
			continue;
		if (rp->wp != NULL)
			server_kill_pane(rp->wp);
	}
}

/* Apply a remotely supplied layout to a local window. */
static void
remote_sync_layout(struct remote *r, struct remote_window *rw)
{
	struct remote_pane	*rp;
	char			*cause = NULL;

	if (rw->w == NULL || rw->layout == NULL)
		return;
	if (window_count_panes(rw->w, 1) == 0)
		return;
	remote_reorder_panes(r, rw);
	remote_prune_panes(r, rw);

	if (layout_parse(rw->w, rw->layout, &cause) != 0) {
		remote_log(r, "window @%u: bad layout: %s", rw->id,
		    cause == NULL ? "unknown" : cause);
		free(cause);
		return;
	}
	RB_FOREACH(rp, remote_panes, &r->panes) {
		if (rp->window_id == rw->id && rp->active && rp->wp != NULL) {
			window_set_active_pane(rw->w, rp->wp, 1);
			break;
		}
	}
	server_redraw_window(rw->w);
}

/* Select the window marked active by the remote. */
static void
remote_set_active_window(struct remote *r)
{
	struct remote_window	*rw;

	if (r->session == NULL)
		return;
	RB_FOREACH(rw, remote_windows, &r->windows) {
		if (rw->wl == NULL)
			continue;
		if (rw->active || rw->want_active) {
			rw->want_active = 0;
			if (r->session->curw != rw->wl)
				session_set_current(r->session, rw->wl);
			break;
		}
	}
}

/* Bootstrap step 0: ask for the new (JSON) layout format. */
static void
remote_bootstrap_refresh(struct remote *r, struct remote_query *q)
{
	struct remote_query	*q2;
	u_int			 sid = q->window_id;

	q2 = remote_query_new(remote_bootstrap_windows, remote_bootstrap_error);
	q2->window_id = sid;
	remote_run(r, q2, "list-windows -t $%u -F \"#{window_id}\\t"
	    "#{window_index}\\t#{window_name}\\t#{window_layout}\\t"
	    "#{window_active}\"", sid);
}

/* Bootstrap step 1: list remote windows. */
static void
remote_bootstrap_windows(struct remote *r, struct remote_query *q)
{
	struct remote_query	*q2;
	char			*line;
	u_int			 sid = q->window_id;

	remote_create_session(r);
	while ((line = evbuffer_readln(r->reply, NULL,
	    EVBUFFER_EOL_LF)) != NULL) {
		remote_parse_window_line(r, line);
		free(line);
	}

	q2 = remote_query_new(remote_bootstrap_panes, remote_bootstrap_error);
	q2->window_id = sid;
	remote_run(r, q2, "list-panes -s -t $%u -F \"#{window_id}\\t"
	    "#{pane_id}\\t#{pane_index}\\t#{pane_active}\\t#{cursor_x}\\t"
	    "#{cursor_y}\\t#{history_limit}\\t#{window_width}\\t"
	    "#{window_height}\"", sid);
}


/* Attach or switch to the local mirror session after bootstrap. */
static void
remote_attach(struct remote *r)
{
	struct cmd_parse_result	*pr;
	struct cmdq_item	*item;
	char			*name, *line;

	if (r->wait_item == NULL || r->session == NULL)
		return;
	if (r->wait_detached) {
		cmdq_continue(r->wait_item);
		r->wait_item = NULL;
		r->wait_client = NULL;
		return;
	}
	name = args_escape(r->session->name);
	if (r->wait_inside)
		xasprintf(&line, "switch-client -t %s", name);
	else
		xasprintf(&line, "attach-session -t %s", name);
	free(name);

	pr = cmd_parse_from_string(line, NULL);
	free(line);
	if (pr->status != CMD_PARSE_SUCCESS) {
		cmdq_error(r->wait_item, "%s",
		    pr->error == NULL ? "parse error" : pr->error);
		free(pr->error);
	} else {
		item = cmdq_get_command(pr->cmdlist, NULL);
		cmd_list_free(pr->cmdlist);
		cmdq_insert_after(r->wait_item, item);
	}
	cmdq_continue(r->wait_item);
	r->wait_item = NULL;
	r->wait_client = NULL;
}

/* Fail the waiting command if the control stream dies during bootstrap. */
static void
remote_fail_wait(struct remote *r, const char *reason)
{
	if (r->wait_item == NULL)
		return;
	cmdq_error(r->wait_item, "remote session failed: %s", reason);
	cmdq_continue(r->wait_item);
	r->wait_item = NULL;
	r->wait_client = NULL;
}

/* Bootstrap step 2: list remote panes. */
static void
remote_bootstrap_panes(struct remote *r, __unused struct remote_query *q)
{
	struct remote_window	*rw, *rw1;
	struct remote_pane	*rp;
	char			*line;

	RB_FOREACH(rw, remote_windows, &r->windows)
		rw->seen = 0;
	RB_FOREACH(rp, remote_panes, &r->panes)
		rp->seen = 0;

	while ((line = evbuffer_readln(r->reply, NULL,
	    EVBUFFER_EOL_LF)) != NULL) {
		remote_parse_pane_line(r, line);
		free(line);
	}

	RB_FOREACH_SAFE(rw, remote_windows, &r->windows, rw1) {
		if (!rw->seen && rw->w != NULL) {
			/* Leave windows with no panes alone for now. */
			continue;
		}
		remote_sync_layout(r, rw);
	}
	remote_set_active_window(r);
	r->booted = 1;
	remote_log(r, "bootstrap complete");
	server_redraw_session(r->session);
	remote_attach(r);
}

/* Bootstrap command failed. */
static void
remote_bootstrap_error(struct remote *r, struct remote_query *q)
{
	char	*line;

	line = evbuffer_readln(r->reply, NULL, EVBUFFER_EOL_LF);
	remote_log(r, "command failed: %s: %s", q->command,
	    line == NULL ? "unknown error" : line);
	free(line);
}

/* Start mirroring a new remote control stream. */
struct remote *
remote_start(const char *remote_command, const char *session_name,
    const char *host, const char *port, const char *token,
    struct cmdq_item *wait_item, struct client *wait_client, int wait_inside,
    int wait_detached)
{
	struct remote	*r;

	if (remote_command == NULL || *remote_command == '\0')
		return (NULL);

	r = xcalloc(1, sizeof *r);
	r->id = next_remote_id++;
	r->reply = evbuffer_new();
	RB_INIT(&r->windows);
	RB_INIT(&r->panes);
	TAILQ_INIT(&r->queries);
	TAILQ_INIT(&r->early);
	if (session_name != NULL) {
		r->session_name = xstrdup(session_name);
		r->name_locked = 1;
	}
	if (host != NULL)
		r->host = xstrdup(host);
	if (port != NULL)
		r->port = xstrdup(port);
	if (token != NULL)
		r->token = xstrdup(token);
	if (wait_item != NULL && wait_client != NULL) {
		r->wait_item = wait_item;
		r->wait_client = wait_client;
		r->wait_inside = wait_inside;
		r->wait_detached = wait_detached;
	}

	r->job = job_run(remote_command, 0, NULL, NULL, NULL, "/tmp",
	    remote_job_update, remote_job_complete, NULL, r,
	    JOB_KEEPWRITE|JOB_NOWAIT,
	    0, 0);
	if (r->job == NULL) {
		remote_log(r, "cannot run: %s", remote_command);
		free(r->session_name);
		evbuffer_free(r->reply);
		free(r);
		return (NULL);
	}
	r->event = job_get_event(r->job);

	TAILQ_INSERT_TAIL(&remotes, r, entry);
	remote_log(r, "started: %s", remote_command);

	return (r);
}

/* Job update callback: parse available output. */
static void
remote_job_update(struct job *job)
{
	struct remote	*r = job_get_data(job);
	struct bufferevent *bev = job_get_event(job);
	char		*line;

	while ((line = evbuffer_readln(bev->input, NULL,
	    EVBUFFER_EOL_LF)) != NULL) {
		remote_read_line(r, line);
		free(line);
	}
}

/* Job completed: stop mirroring. */
static void
remote_job_complete(struct job *job)
{
	struct remote	*r = job_get_data(job);

	remote_log(r, "remote process exited");
	r->job = NULL;
	r->event = NULL;
	remote_fail_wait(r, "control connection exited");
	remote_free(r);
}

/* Read one protocol line. */
static void
remote_read_line(struct remote *r, char *line)
{
	log_debug("remote #%u: %s", r->id, line);

	if (r->reply_number != 0) {
		if (strncmp(line, "%end ", 5) == 0) {
			remote_reply_end(r, line, 0);
			r->reply_number = 0;
			return;
		}
		if (strncmp(line, "%error ", 7) == 0) {
			remote_reply_end(r, line, 1);
			r->reply_number = 0;
			return;
		}
		evbuffer_add(r->reply, line, strlen(line));
		evbuffer_add(r->reply, "\n", 1);
		return;
	}
	if (strncmp(line, "%begin ", 7) == 0) {
		remote_reply_begin(r, line);
		return;
	}
	if (line[0] == '%')
		remote_dispatch_event(r, line);
	else
		remote_log(r, "protocol error: %s", line);
}

static void
remote_reply_begin(struct remote *r, char *line)
{
	long	t;
	u_int	n, f;

	if (sscanf(line, "%%begin %ld %u %u", &t, &n, &f) != 3) {
		remote_log(r, "bad %%begin: %s", line);
		return;
	}
	r->reply_time = t;
	r->reply_number = n;
	r->reply_flags = f;
}

static void
remote_reply_end(struct remote *r, char *line, int error)
{
	struct remote_query	*q;
	void			(*cb)(struct remote *, struct remote_query *);
	long			 t;
	u_int			 n, f;

	if (sscanf(line, "%*s %ld %u %u", &t, &n, &f) != 3) {
		remote_log(r, "bad reply footer: %s", line);
		return;
	}
	if (t != r->reply_time || n != r->reply_number) {
		remote_log(r, "reply metadata mismatch");
		return;
	}

	if (f & 1) {
		q = TAILQ_FIRST(&r->queries);
		if (q == NULL)
			remote_log(r, "no query for reply %u", n);
		else {
			TAILQ_REMOVE(&r->queries, q, entry);
			cb = error ? q->error : q->done;
			if (cb != NULL)
				cb(r, q);
			remote_query_free(q);
		}
	}
	evbuffer_drain(r->reply, EV_SIZE_MAX);
}

/* Dispatch a notification line. */
static void
remote_dispatch_event(struct remote *r, char *line)
{
	char	*p, *rest, *copy, *layout;
	u_int	 id, id2;

	if (strncmp(line, "%output ", 8) == 0) {
		p = line + 8;
		if (*p != '%')
			return;
		id = strtoul(p + 1, &rest, 10);
		if (*rest == ' ')
			rest++;
		remote_output(r, id, rest);
		return;
	}
	if (strncmp(line, "%extended-output ", 17) == 0) {
		/* Not implemented: flow control is not used by this prototype. */
		return;
	}
	if (strncmp(line, "%layout-change ", 15) == 0) {
		p = line + 15;
		if (*p != '@')
			return;
		id = strtoul(p + 1, &rest, 10);
		if (*rest != ' ')
			return;
		rest++;
		layout = strsep(&rest, " ");
		remote_layout_change(r, id, layout);
		return;
	}
	if (strncmp(line, "%window-add ", 12) == 0) {
		id = strtoul(line + 12 + 1, NULL, 10);
		remote_window_add(r, id);
		return;
	}
	if (strncmp(line, "%window-close ", 14) == 0) {
		id = strtoul(line + 14 + 1, NULL, 10);
		remote_window_close(r, id);
		return;
	}
	if (strncmp(line, "%window-renamed ", 16) == 0) {
		p = line + 16;
		if (*p != '@')
			return;
		id = strtoul(p + 1, &rest, 10);
		if (*rest == ' ')
			rest++;
		remote_window_renamed(r, id, rest);
		return;
	}
	if (strncmp(line, "%window-pane-changed ", 21) == 0) {
		p = line + 21;
		if (*p != '@')
			return;
		id = strtoul(p + 1, &rest, 10);
		if (*rest != ' ' || rest[1] != '%')
			return;
		id2 = strtoul(rest + 2, NULL, 10);
		remote_window_pane_changed(r, id, id2);
		return;
	}
	if (strncmp(line, "%session-window-changed ", 24) == 0) {
		p = line + 24;
		if (*p != '$')
			return;
		id = strtoul(p + 1, &rest, 10);
		if (*rest != ' ' || rest[1] != '@')
			return;
		id2 = strtoul(rest + 2, NULL, 10);
		remote_session_window_changed(r, id, id2);
		return;
	}
	if (strncmp(line, "%session-changed ", 17) == 0) {
		p = line + 17;
		if (*p != '$')
			return;
		id = strtoul(p + 1, &rest, 10);
		if (*rest == ' ')
			rest++;
		copy = xstrdup(rest);
		remote_session_changed(r, id, copy);
		free(copy);
		return;
	}
	if (strncmp(line, "%session-renamed ", 17) == 0)
		return;
	if (strncmp(line, "%sessions-changed", 17) == 0)
		return;
	if (strncmp(line, "%pane-mode-changed ", 19) == 0)
		return;
	if (strncmp(line, "%client-session-changed ", 24) == 0)
		return;
	if (strncmp(line, "%exit", 5) == 0) {
		remote_log(r, "remote requested exit");
		remote_exit(r);
		return;
	}
	/* Unknown notification: ignore. */
}

/* Remote session was attached or changed. */
static void
remote_session_changed(struct remote *r, u_int id, char *name)
{
	struct remote_query	*q;

	remote_log(r, "session changed: $%u %s", id, name);
	if (r->session_id == id && r->booted)
		return;
	free(r->remote_name);
	r->remote_name = xstrdup(name);

	remote_reset(r);
	if (!r->name_locked) {
		free(r->session_name);
		r->session_name = xstrdup(name);
	}
	r->session_id = id;
	r->booted = 0;
	r->booting = 1;

	q = remote_query_new(remote_bootstrap_refresh, remote_bootstrap_error);
	q->window_id = id;
	remote_run(r, q, "refresh-client -f new-layouts");
}

/* Remote window was added. */
static void
remote_window_add(struct remote *r, u_int id)
{
	struct remote_query	*q;

	remote_log(r, "window add: @%u", id);
	remote_window_get(r, id, 1);

	q = remote_query_new(remote_window_add_done, remote_bootstrap_error);
	q->window_id = id;
	remote_run(r, q, "list-windows -t @%u -F \"#{window_id}\\t"
	    "#{window_index}\\t#{window_name}\\t#{window_layout}\\t"
	    "#{window_active}\"", id);
}

static void
remote_window_add_done(struct remote *r, struct remote_query *q)
{
	struct remote_query	*q2;
	struct remote_window	*rw;
	char			*line;
	u_int			 id = q->window_id;

	if (r->session == NULL)
		remote_create_session(r);
	while ((line = evbuffer_readln(r->reply, NULL,
	    EVBUFFER_EOL_LF)) != NULL) {
		remote_parse_window_line(r, line);
		free(line);
	}
	rw = remote_window_get(r, id, 0);
	if (rw == NULL || rw->w == NULL)
		return;

	q2 = remote_query_new(remote_window_panes_done, remote_bootstrap_error);
	q2->window_id = id;
	remote_run(r, q2, "list-panes -t @%u -F \"#{window_id}\\t#{pane_id}\\t"
	    "#{pane_index}\\t#{pane_active}\\t#{cursor_x}\\t#{cursor_y}\\t"
	    "#{history_limit}\\t#{window_width}\\t#{window_height}\"", id);
}

static void
remote_window_panes_done(struct remote *r, struct remote_query *q)
{
	struct remote_window	*rw;
	struct remote_pane	*rp;
	char			*line;
	u_int			 id = q->window_id;

	rw = remote_window_get(r, id, 0);
	if (rw == NULL)
		return;
	rw->seen = 0;
	RB_FOREACH(rp, remote_panes, &r->panes) {
		if (rp->window_id == id)
			rp->seen = 0;
	}
	while ((line = evbuffer_readln(r->reply, NULL,
	    EVBUFFER_EOL_LF)) != NULL) {
		remote_parse_pane_line(r, line);
		free(line);
	}
	remote_sync_layout(r, rw);
	if (rw->want_active)
		remote_set_active_window(r);
	server_redraw_session(r->session);
}

/* Remote window was closed. */
static void
remote_window_close(struct remote *r, u_int id)
{
	struct remote_window	*rw;
	struct session		*s = r->session;

	remote_log(r, "window close: @%u", id);
	rw = remote_window_get(r, id, 0);
	if (rw == NULL)
		return;
	RB_REMOVE(remote_windows, &r->windows, rw);

	if (s != NULL && rw->wl != NULL) {
		if (session_detach(s, rw->wl) == 1) {
			/* Last window: destroy the whole mirror session. */
			remote_destroy_session(r);
		} else
			server_redraw_session_group(s);
	}
	free(rw->layout);
	free(rw->name);
	free(rw);
}

/* Remote window was renamed. */
static void
remote_window_renamed(struct remote *r, u_int id, char *name)
{
	struct remote_window	*rw;

	rw = remote_window_get(r, id, 0);
	if (rw == NULL)
		return;
	free(rw->name);
	rw->name = xstrdup(name);
	if (rw->w != NULL)
		window_set_name(rw->w, name, 0);
	if (r->session != NULL)
		server_status_session(r->session);
}

/* Remote active pane changed. */
static void
remote_window_pane_changed(struct remote *r, u_int id, u_int pane_id)
{
	struct remote_window	*rw;
	struct remote_pane	*rp;

	rw = remote_window_get(r, id, 0);
	rp = remote_pane_get(r, pane_id, 0);
	if (rw == NULL || rp == NULL || rw->w == NULL || rp->wp == NULL)
		return;
	if (rp->wp->window != rw->w)
		return;
	window_set_active_pane(rw->w, rp->wp, 1);
}

/* Remote current window changed. */
static void
remote_session_window_changed(struct remote *r, u_int session_id, u_int id)
{
	struct remote_window	*rw;

	if (r->session_id != session_id)
		return;
	rw = remote_window_get(r, id, 0);
	if (rw == NULL)
		return;
	if (rw->wl == NULL) {
		rw->want_active = 1;
		return;
	}
	if (r->session != NULL)
		session_set_current(r->session, rw->wl);
}

/* Remote layout changed. */
static void
remote_layout_change(struct remote *r, u_int id, char *layout)
{
	struct remote_window	*rw;
	struct remote_query	*q;

	rw = remote_window_get(r, id, 0);
	if (rw == NULL || rw->w == NULL)
		return;
	free(rw->layout);
	rw->layout = xstrdup(layout);

	q = remote_query_new(remote_window_panes_done, remote_bootstrap_error);
	q->window_id = id;
	remote_run(r, q, "list-panes -t @%u -F \"#{window_id}\\t#{pane_id}\\t"
	    "#{pane_index}\\t#{pane_active}\\t#{cursor_x}\\t#{cursor_y}\\t"
	    "#{history_limit}\\t#{window_width}\\t#{window_height}\"", id);
}

/* Remote requested exit. */
static void
remote_exit(struct remote *r)
{
	remote_log(r, "remote exit");
	r->booted = 0;
}

/* Clear local state (but keep the remote connection). */
static void
remote_reset(struct remote *r)
{
	struct remote_query	*q;
	struct remote_early	*re;

	while ((q = TAILQ_FIRST(&r->queries)) != NULL) {
		TAILQ_REMOVE(&r->queries, q, entry);
		remote_query_free(q);
	}
	while ((re = TAILQ_FIRST(&r->early)) != NULL) {
		TAILQ_REMOVE(&r->early, re, entry);
		evbuffer_free(re->data);
		free(re);
	}
	evbuffer_drain(r->reply, EV_SIZE_MAX);
	remote_destroy_session(r);
}

/* Free a remote and its local session. */
static void
remote_free(struct remote *r)
{
	remote_log(r, "free");
	remote_reset(r);
	free(r->session_name);
	free(r->remote_name);
	free(r->host);
	free(r->port);
	free(r->token);
	evbuffer_free(r->reply);
	TAILQ_REMOVE(&remotes, r, entry);
	free(r);
}

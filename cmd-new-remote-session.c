/* $OpenBSD$ */

/*
 * Copyright (c) 2026 The tmux authors
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/wait.h>

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tmux.h"

/*
 * Create a local session whose initial pane is an SSH connection running a
 * remote tmux client. The local server owns the pty, so input, output,
 * resize, detach and redraw all use the normal tmux pane paths.
 */

/*
 * The helper runs locally in the pane. It establishes a persistent SSH
 * control connection, copies the local tmux configuration and plugins to a
 * private directory on the remote, then execs the interactive SSH client.
 *
 * Arguments: host port control-path hash remote-command force
 */
static const char remote_helper[] =
	"set -e\n"
	"host=$1; port=$2; ctl=$3; hash=$4; cmd=$5; force=$6\n"
	"mkdir -p \"${ctl%/*}\" 2>/dev/null || :\n"
	"chmod 700 \"${ctl%/*}\" 2>/dev/null || :\n"
	"set -- -o ControlMaster=auto -o ControlPath=\"$ctl\" "
	"-o ControlPersist=yes -o ServerAliveInterval=15 "
	"-o ServerAliveCountMax=3 -o TCPKeepAlive=yes "
	"-o ConnectTimeout=10\n"
	"[ -n \"$port\" ] && set -- \"$@\" -p \"$port\"\n"
	"if [ \"$force\" = 1 ]; then\n"
	"  ssh -o ControlPath=\"$ctl\" -O exit \"$host\" "
	">/dev/null 2>&1 || :\n"
	"  rm -f \"$ctl\" 2>/dev/null || :\n"
	"fi\n"
	"files=\n"
	"[ -d \"$HOME/.tmux\" ] && files=\"$files .tmux\"\n"
	"[ -f \"$HOME/.tmux.conf\" ] && files=\"$files .tmux.conf\"\n"
	"[ -d \"$HOME/.config/tmux\" ] && files=\"$files .config/tmux\"\n"
	"if [ -n \"$files\" ] && command -v tar >/dev/null 2>&1; then\n"
	"  tar -C \"$HOME\" -cf - $files 2>/dev/null | ssh \"$@\" \"$host\" "
	"\"mkdir -p \\\"\\$HOME/.cache/tmux-remote/$hash\\\" && "
	"tar -C \\\"\\$HOME/.cache/tmux-remote/$hash\\\" -xf -\" "
	"2>/dev/null || :\n"
	"fi\n"
	"exec ssh -tt \"$@\" \"$host\" \"$cmd\"\n";

/*
 * The remote side of the helper. It chooses the synced configuration (if
 * any), rewrites local home references to the cache directory, and runs the
 * requested tmux new-session command with any remaining arguments.
 *
 * Arguments from sh(1): hash tmux-arguments...
 */
static const char remote_bootstrap[] =
	"hash=$1\n"
	"shift\n"
	"d=\"$HOME/.cache/tmux-remote/$hash\"\n"
	"c=\"$d/.tmux.conf\"\n"
	"[ -f \"$c\" ] || c=\"$d/.config/tmux/tmux.conf\"\n"
	"if [ -f \"$c\" ]; then\n"
	"  sed -e 's|~/.tmux|'\"$d\"'/.tmux|g' "
	"-e 's|\\$HOME/.tmux|'\"$d\"'/.tmux|g' "
	"-e 's|~/.config/tmux|'\"$d\"'/.config/tmux|g' "
	"\"$c\" > \"$c.tmp\" && mv \"$c.tmp\" \"$c\"\n"
	"  export TMUX_PLUGIN_MANAGER_PATH=\"$d/.tmux/plugins\"\n"
	"  export TERM=xterm-256color\n"
	"  exec tmux -f \"$c\" \"$@\"\n"
	"fi\n"
	"export TERM=xterm-256color\n"
	"exec tmux \"$@\"\n";

struct cmd_new_remote_session_data {
	u_int	 window;
	char	*host;
	char	*name;
	char	*port;
};

struct cmd_new_remote_check_data {
	struct cmdq_item	*item;
	struct job		*job;
	struct event		 timer;
	int			 timed_out;
	char			*respawn;
	char			*show;
};

static enum cmd_retval	cmd_new_remote_session_exec(struct cmd *,
			    struct cmdq_item *);
static enum cmd_retval	cmd_new_remote_session_done(struct cmdq_item *,
			    void *);
static void		cmd_new_remote_check_timeout(int, short, void *);
static void		cmd_new_remote_check_free(void *);

const struct cmd_entry cmd_new_remote_session_entry = {
	.name = "new-remote-session",
	.description = "Create a new remote session.",
	.alias = "newr",

	.args = { "Ac:dDe:EF:f:H:n:Pp:Rs:t:x:Xy:", 0, -1, NULL },
	.usage = "[-AdDEPRX] [-c start-directory] [-e environment] "
		 "[-F format] [-f flags] [-H host] [-n window-name] "
		 "[-p port] [-s session-name] "
		 CMD_TARGET_SESSION_USAGE " [-x width] [-y height] "
		 "[user@host] [shell-command [argument ...]]",

	.target = { 't', CMD_FIND_SESSION, CMD_FIND_CANFAIL },

	.flags = CMD_STARTSERVER,
	.exec = cmd_new_remote_session_exec
};

/* Append a string to a dynamically allocated buffer. */
static void
remote_cat(char **buf, size_t *len, const char *s)
{
	size_t	n = strlen(s);

	*buf = xrealloc(*buf, *len + n + 1);
	memcpy(*buf + *len, s, n + 1);
	*len += n;
}

/* Quote a string for a POSIX shell. */
static char *
remote_sh_quote(const char *s)
{
	const char	*p;
	char		*out, *q;
	size_t		 len = 2;

	for (p = s; *p != '\0'; p++)
		len += (*p == '\'') ? 4 : 1;
	out = xmalloc(len + 1);

	q = out;
	*q++ = '\'';
	for (p = s; *p != '\0'; p++) {
		if (*p == '\'') {
			memcpy(q, "'\\''", 4);
			q += 4;
		} else
			*q++ = *p;
	}
	*q++ = '\'';
	*q = '\0';
	return (out);
}

/* A stable per-host/session identifier used for sockets and cache paths. */
static char *
remote_hash(const char *host, const char *port, const char *name)
{
	uint64_t	 h = UINT64_C(1469598103934665603);
	const char	*p;
	char		*out;

	for (p = host; *p != '\0'; p++) {
		h ^= (unsigned char)*p;
		h *= UINT64_C(1099511628211);
	}
	h ^= '|';
	h *= UINT64_C(1099511628211);
	if (port != NULL) {
		for (p = port; *p != '\0'; p++) {
			h ^= (unsigned char)*p;
			h *= UINT64_C(1099511628211);
		}
	}
	h ^= '|';
	h *= UINT64_C(1099511628211);
	for (p = name; *p != '\0'; p++) {
		h ^= (unsigned char)*p;
		h *= UINT64_C(1099511628211);
	}
	xasprintf(&out, "%016llx", (unsigned long long)h);
	return (out);
}

/* Default local and remote session name derived from the host. */
static char *
remote_default_name(const char *host)
{
	const char	*p = strrchr(host, '@');
	char		*out, *q;

	if (p != NULL)
		p++;
	else
		p = host;

	out = xmalloc(strlen(p) + sizeof "remote-");
	memcpy(out, "remote-", 7);
	q = out + 7;
	for (; *p != '\0'; p++) {
		if (isalnum((unsigned char)*p) || *p == '.' || *p == '-' ||
		    *p == '_')
			*q++ = *p;
		else if (*p == ':' || *p == '[' || *p == ']')
			continue;
		else
			*q++ = '_';
	}
	if (q == out + 7)
		*q++ = 'h';
	*q = '\0';
	return (out);
}

/* Get a string user option from an options set, or NULL if not set. */
static char *
remote_option(struct options *oo, const char *name)
{
	struct options_entry	*o;
	char			*value;

	o = options_get_only(oo, name);
	if (o == NULL)
		return (NULL);
	value = options_to_string(o, NULL, 0);
	if (*value == '\0') {
		free(value);
		return (NULL);
	}
	return (value);
}

/* Get a string session user option, or NULL if not set. */
static char *
remote_session_option(struct session *s, const char *name)
{
	return (remote_option(s->options, name));
}

/* Get a string pane user option, or NULL if not set. */
static char *
remote_pane_option(struct window_pane *wp, const char *name)
{
	return (remote_option(wp->options, name));
}

/* Does this pane belong to the remote host and session? */
static int
remote_pane_matches(struct window_pane *wp, const char *host, const char *name)
{
	char	*h, *n;
	int	 match = 0;

	h = remote_pane_option(wp, "@remote_host");
	n = remote_pane_option(wp, "@remote_session");
	if (h != NULL && n != NULL && strcmp(h, host) == 0 &&
	    strcmp(n, name) == 0)
		match = 1;
	free(h);
	free(n);
	return (match);
}

/* Find an existing remote modal pane. */
static struct window_pane *
remote_find_pane(const char *host, const char *name)
{
	struct window_pane	*wp;

	RB_FOREACH(wp, window_pane_tree, &all_window_panes) {
		if (remote_pane_matches(wp, host, name))
			return (wp);
	}
	return (NULL);
}

/* Add a flag and optional value to a tmux argument vector. */
static void
remote_append_option(int *argc, char ***argv, const char *flag,
    const char *value)
{
	cmd_append_argv(argc, argv, flag);
	if (value != NULL)
		cmd_append_argv(argc, argv, value);
}

/*
 * Build the remote tmux new-session argument vector. Every remote session is
 * attach-or-create, so an existing remote session is reattached.
 */
static void
remote_build_remote_argv(struct args *args, const char *name, int offset,
    int argc, char **argv, int *out_argc, char ***out_argv)
{
	struct args_value	*av;
	int			 i;
	int			 n = 0;
	char		       **v = NULL;

	remote_append_option(&n, &v, "new-session", NULL);
	remote_append_option(&n, &v, "-A", NULL);
	if (args_has(args, 'c'))
		remote_append_option(&n, &v, "-c", args_get(args, 'c'));
	if (args_has(args, 'D'))
		remote_append_option(&n, &v, "-D", NULL);
	av = args_first_value(args, 'e');
	while (av != NULL) {
		remote_append_option(&n, &v, "-e", av->string);
		av = args_next_value(av);
	}
	if (args_has(args, 'E'))
		remote_append_option(&n, &v, "-E", NULL);
	if (args_has(args, 'F'))
		remote_append_option(&n, &v, "-F", args_get(args, 'F'));
	if (args_has(args, 'f'))
		remote_append_option(&n, &v, "-f", args_get(args, 'f'));
	if (args_has(args, 'n'))
		remote_append_option(&n, &v, "-n", args_get(args, 'n'));
	if (args_has(args, 'P'))
		remote_append_option(&n, &v, "-P", NULL);
	remote_append_option(&n, &v, "-s", name);
	if (args_has(args, 't'))
		remote_append_option(&n, &v, "-t", args_get(args, 't'));
	if (args_has(args, 'x'))
		remote_append_option(&n, &v, "-x", args_get(args, 'x'));
	if (args_has(args, 'X'))
		remote_append_option(&n, &v, "-X", NULL);
	if (args_has(args, 'y'))
		remote_append_option(&n, &v, "-y", args_get(args, 'y'));
	if (offset < argc) {
		remote_append_option(&n, &v, "--", NULL);
		for (i = offset; i < argc; i++)
			cmd_append_argv(&n, &v, argv[i]);
	}

	*out_argc = n;
	*out_argv = v;
}

/* Build the complete command string sent to the remote shell. */
static char *
remote_build_command(const char *hash, int argc, char **argv)
{
	char	*out = NULL, *quoted;
	size_t	 len = 0;
	int	 i;

	remote_cat(&out, &len, "sh -c ");
	quoted = remote_sh_quote(remote_bootstrap);
	remote_cat(&out, &len, quoted);
	free(quoted);
	remote_cat(&out, &len, " sh ");
	quoted = remote_sh_quote(hash);
	remote_cat(&out, &len, quoted);
	free(quoted);
	for (i = 0; i < argc; i++) {
		remote_cat(&out, &len, " ");
		quoted = remote_sh_quote(argv[i]);
		remote_cat(&out, &len, quoted);
		free(quoted);
	}
	return (out);
}

/* Build the local argv which runs the helper in the pane. */
static char **
remote_build_helper_argv(const char *host, const char *port, const char *ctl,
    const char *hash, const char *command, int force, int *argc)
{
	char	**argv = NULL;

	*argc = 0;
	cmd_append_argv(argc, &argv, "sh");
	cmd_append_argv(argc, &argv, "-c");
	cmd_append_argv(argc, &argv, remote_helper);
	cmd_append_argv(argc, &argv, "sh");
	cmd_append_argv(argc, &argv, host);
	cmd_append_argv(argc, &argv, port == NULL ? "" : port);
	cmd_append_argv(argc, &argv, ctl);
	cmd_append_argv(argc, &argv, hash);
	cmd_append_argv(argc, &argv, command);
	cmd_append_argv(argc, &argv, force ? "1" : "0");
	return (argv);
}

/* Insert a command string immediately after an item in the queue. */
static struct cmdq_item * printflike(2, 3)
remote_queue(struct cmdq_item *after, const char *fmt, ...)
{
	va_list			 ap;
	struct cmd_parse_result	*pr;
	struct cmdq_item	*item;
	char			*line;

	va_start(ap, fmt);
	xvasprintf(&line, fmt, ap);
	va_end(ap);

	pr = cmd_parse_from_string(line, NULL);
	free(line);
	if (pr->status != CMD_PARSE_SUCCESS) {
		cmdq_error(after, "%s",
		    pr->error == NULL ? "parse error" : pr->error);
		free(pr->error);
		return (NULL);
	}
	item = cmdq_get_command(pr->cmdlist, NULL);
	cmd_list_free(pr->cmdlist);
	return (cmdq_insert_after(after, item));
}

/* Build a respawn-pane command for a remote pane. */
static char *
remote_build_respawn(struct window_pane *wp, int argc, char **argv)
{
	char	*command, *line;

	command = cmd_stringify_argv(argc, argv);
	xasprintf(&line, "respawn-pane -k -t %%%u -- %s", wp->id, command);
	free(command);
	return (line);
}

/* Queue a respawn of a remote pane with a fresh helper. */
static struct cmdq_item *
remote_queue_respawn(struct cmdq_item *after, struct window_pane *wp,
    int argc, char **argv)
{
	char			*line;
	struct cmdq_item	*item;

	line = remote_build_respawn(wp, argc, argv);
	item = remote_queue(after, "%s", line);
	free(line);
	return (item);
}

/* Build a command to attach or switch to an existing local session. */
static char *
remote_build_attach(const char *name, struct args *args, int inside)
{
	char	*escaped, *flags = NULL, *tmp, *line;
	size_t	 len = 0;

	escaped = args_escape(name);
	if (inside) {
		xasprintf(&line, "switch-client -t %s", escaped);
	} else {
		remote_cat(&flags, &len, "");
		if (args_has(args, 'D'))
			remote_cat(&flags, &len, " -d");
		if (args_has(args, 'X'))
			remote_cat(&flags, &len, " -x");
		if (args_has(args, 'E'))
			remote_cat(&flags, &len, " -E");
		if (args_has(args, 'f')) {
			tmp = args_escape(args_get(args, 'f'));
			remote_cat(&flags, &len, " -f ");
			remote_cat(&flags, &len, tmp);
			free(tmp);
		}
		xasprintf(&line, "attach-session%s -t %s", flags, escaped);
		free(flags);
	}
	free(escaped);
	return (line);
}

/* Build a command to show an existing modal pane to the current client. */
static char *
remote_build_show(struct window_pane *wp, struct session *current)
{
	struct cmd_find_state	 fs;
	char			*name, *line = NULL;
	size_t			 len = 0;

	if (cmd_find_from_pane(&fs, wp, 0) != 0)
		fs.s = NULL;

	remote_cat(&line, &len, "");
	if (current == NULL || fs.s != current) {
		if (fs.s != NULL) {
			name = args_escape(fs.s->name);
			remote_cat(&line, &len, "switch-client -t ");
			remote_cat(&line, &len, name);
			remote_cat(&line, &len, "; ");
			free(name);
		}
	}
	xasprintf(&name, "select-window -t @%u", wp->window->id);
	remote_cat(&line, &len, name);
	free(name);
	return (line);
}

/* Mark a pane as handling a remote session. */
static void
remote_mark_pane(struct window_pane *wp, struct cmd_new_remote_session_data *d)
{
	options_set_string(wp->options, "@remote_host", 0, "%s", d->host);
	options_set_string(wp->options, "@remote_session", 0, "%s", d->name);
	if (d->port != NULL)
		options_set_string(wp->options, "@remote_port", 0, "%s",
		    d->port);
	options_set_number(wp->options, "remain-on-exit", 1);
	options_set_string(wp->options, "remain-on-exit-format", 0,
	    "disconnected from %s", d->host);
}

/* Set the metadata and lifetime options once the new session exists. */
static enum cmd_retval
cmd_new_remote_session_done(__unused struct cmdq_item *item, void *data)
{
	struct cmd_new_remote_session_data	*d = data;
	struct session				*s;
	struct winlink				*wl;
	struct window				*w;
	struct window_pane			*wp;

	if (d->window != UINT_MAX) {
		w = window_find_by_id(d->window);
		if (w != NULL) {
			wp = w->modal;
			if (wp == NULL)
				wp = w->active;
			if (wp != NULL)
				remote_mark_pane(wp, d);
			options_set_string(w->options, "@remote_host", 0, "%s",
			    d->host);
			options_set_string(w->options, "@remote_session", 0,
			    "%s", d->name);
		}
	} else {
		s = session_find(d->name);
		if (s != NULL) {
			options_set_string(s->options, "@remote_host", 0, "%s",
			    d->host);
			options_set_string(s->options, "@remote_session", 0,
			    "%s", d->name);
			if (d->port != NULL)
				options_set_string(s->options, "@remote_port",
				    0, "%s", d->port);
			options_set_number(s->options, "destroy-unattached", 0);
			wl = s->curw;
			if (wl != NULL && (wp = wl->window->active) != NULL)
				remote_mark_pane(wp, d);
		}
	}

	free(d->host);
	free(d->name);
	free(d->port);
	free(d);
	return (CMD_RETURN_NORMAL);
}

/* Timer callback for a hung SSH liveness check. */
static void
cmd_new_remote_check_timeout(__unused int fd, __unused short events, void *data)
{
	struct cmd_new_remote_check_data	*cd = data;

	/*
	 * job_free() calls the free callback, which queues the reconnect and
	 * releases the command. Do not touch cd after this.
	 */
	cd->timed_out = 1;
	job_free(cd->job);
}

/* Job free callback: continue the attach once the liveness check is done. */
static void
cmd_new_remote_check_free(void *data)
{
	struct cmd_new_remote_check_data	*cd = data;
	struct cmdq_item			*after = cd->item, *next;
	int					 status, ok;

	evtimer_del(&cd->timer);
	status = job_get_status(cd->job);
	ok = !cd->timed_out && WIFEXITED(status) && WEXITSTATUS(status) == 0;
	log_debug("%s: ok=%d timed_out=%d status=%d", __func__, ok,
	    cd->timed_out, status);

	if (!ok && cd->respawn != NULL) {
		next = remote_queue(after, "%s", cd->respawn);
		if (next != NULL)
			after = next;
	}
	if (cd->show != NULL)
		remote_queue(after, "%s", cd->show);
	cmdq_continue(cd->item);

	free(cd->respawn);
	free(cd->show);
	free(cd);
}

/*
 * Start an asynchronous SSH liveness check. If the control connection is
 * gone or stops responding within the timeout, cd->respawn (built with force
 * reconnect) runs before cd->show.
 */
static int
remote_start_check(struct cmdq_item *item, struct session *s,
    const char *host, const char *ctl, char *respawn, char *show)
{
	struct cmd_new_remote_check_data	*cd;
	struct timeval				 tv = { 5, 0 };
	char					*command = NULL, *quoted;
	size_t					 len = 0;

	cd = xcalloc(1, sizeof *cd);
	cd->item = item;
	cd->respawn = respawn;
	cd->show = show;

	remote_cat(&command, &len, "ssh -O check -o ControlPath=");
	quoted = remote_sh_quote(ctl);
	remote_cat(&command, &len, quoted);
	free(quoted);
	remote_cat(&command, &len, " ");
	quoted = remote_sh_quote(host);
	remote_cat(&command, &len, quoted);
	free(quoted);
	remote_cat(&command, &len,
	    " && ssh -o BatchMode=yes -o ControlMaster=no -o ControlPath=");
	quoted = remote_sh_quote(ctl);
	remote_cat(&command, &len, quoted);
	free(quoted);
	remote_cat(&command, &len, " ");
	quoted = remote_sh_quote(host);
	remote_cat(&command, &len, quoted);
	free(quoted);
	remote_cat(&command, &len, " true");

	log_debug("%s: %s", __func__, command);
	cd->job = job_run(command, 0, NULL, NULL, s, NULL, NULL, NULL,
	    cmd_new_remote_check_free, cd, 0, -1, -1);
	free(command);
	if (cd->job == NULL) {
		free(cd);
		return (-1);
	}

	evtimer_set(&cd->timer, cmd_new_remote_check_timeout, cd);
	evtimer_add(&cd->timer, &tv);
	return (0);
}

static enum cmd_retval
cmd_new_remote_session_exec(struct cmd *self, struct cmdq_item *item)
{
	struct args			*args = cmd_get_args(self);
	struct cmd_find_state		 fs;
	struct client			*c = cmdq_get_client(item);
	struct cmdq_item		*after = item, *next;
	struct session			*s;
	struct winlink			*wl;
	struct window_pane		*wp, *mwp;
	struct environ_entry		*ee;
	struct cmd_new_remote_session_data *data;
	const char			*value, *errstr;
	char				*host = NULL, *port = NULL;
	char				*ename, *name = NULL, *hash = NULL;
	char				*ctl = NULL;
	const char			*runtime;
	char				*remote = NULL, *ssh = NULL;
	char				*nflags = NULL, *rhost = NULL;
	char				*escaped, *tmp, *title = NULL;
	char				*respawn = NULL, *show = NULL;
	size_t				 nlen = 0;
	char				**argv = NULL, **rargv = NULL, **hargv = NULL;
	char				**fargv = NULL;
	int				 argc = 0, rargc = 0, hargc = 0;
	int				 fargc = 0;
	int				 offset = 0, inside = 0, force, detached;
	int				 dead = 0;
	enum cmd_retval			 retval = CMD_RETURN_NORMAL;

	force = args_has(args, 'R');
	detached = args_has(args, 'd');

	args_to_vector(args, &argc, &argv);
	if ((value = args_get(args, 'H')) != NULL) {
		host = format_single(item, value, c, NULL, NULL, NULL);
	} else if (argc > 0) {
		host = format_single(item, argv[0], c, NULL, NULL, NULL);
		offset = 1;
	} else {
		cmdq_error(item, "no remote host");
		goto fail;
	}
	if (*host == '\0' || *host == '-' ||
	    strpbrk(host, " \t\r\n") != NULL) {
		cmdq_error(item, "invalid remote host: %s", host);
		goto fail;
	}
	if (offset < argc && strcmp(argv[offset], "--") == 0)
		offset++;
	if ((value = args_get(args, 'p')) != NULL) {
		port = format_single(item, value, c, NULL, NULL, NULL);
		(void)strtonum(port, 1, 65535, &errstr);
		if (errstr != NULL) {
			cmdq_error(item, "port %s", errstr);
			goto fail;
		}
	}
	if ((value = args_get(args, 's')) != NULL) {
		ename = format_single(item, value, c, NULL, NULL, NULL);
		if (!check_name(ename)) {
			cmdq_error(item, "invalid session name: %s", ename);
			free(ename);
			goto fail;
		}
		name = clean_name(ename, 0);
		free(ename);
	} else
		name = remote_default_name(host);

	/*
	 * Find the session containing the client's pane, if the command was
	 * run from inside this tmux server, and use it to switch the outer
	 * client later. The TMUX environment variable must name this server,
	 * otherwise the client is only inheriting TMUX from another server.
	 */
	if (c != NULL && (ee = environ_find(c->environ, "TMUX")) != NULL) {
		tmp = xstrdup(ee->value);
		if ((escaped = strchr(tmp, ',')) != NULL)
			*escaped = '\0';
		if (strcmp(tmp, socket_path) == 0 &&
		    (ee = environ_find(c->environ, "TMUX_PANE")) != NULL) {
			wp = window_pane_find_by_id_str(ee->value);
			if (wp != NULL && cmd_find_from_pane(&fs, wp, 0) == 0)
				inside = 1;
		}
		free(tmp);
	}

	hash = remote_hash(host, port, name);
	runtime = getenv("XDG_RUNTIME_DIR");
	if (runtime == NULL || *runtime == '\0')
		runtime = "/tmp";
	xasprintf(&ctl, "%s/tmux-remote-%u/%s.sock", runtime,
	    (u_int)getuid(), hash);

	remote_build_remote_argv(args, name, offset, argc, argv, &rargc,
	    &rargv);
	remote = remote_build_command(hash, rargc, rargv);
	hargv = remote_build_helper_argv(host, port, ctl, hash, remote, force,
	    &hargc);

	/*
	 * When run from inside tmux, show the remote session in a full-size
	 * modal pane in the invoking window rather than switching sessions.
	 * Reuse an existing remote modal pane when there is one.
	 */
	if (inside && !detached) {
		mwp = remote_find_pane(host, name);
		if (mwp != NULL) {
			dead = window_pane_exited(mwp);
			show = remote_build_show(mwp, fs.s);
			if (!force && !dead) {
				fargv = remote_build_helper_argv(host, port,
				    ctl, hash, remote, 1, &fargc);
				respawn = remote_build_respawn(mwp, fargc,
				    fargv);
				if (remote_start_check(item, fs.s, host, ctl,
				    respawn, show) == 0) {
					respawn = NULL;
					show = NULL;
					retval = CMD_RETURN_WAIT;
				} else {
					next = remote_queue(after, "%s",
					    respawn);
					if (next != NULL)
						after = next;
					next = remote_queue(after, "%s", show);
					if (next != NULL)
						after = next;
				}
			} else {
				next = remote_queue_respawn(after, mwp, hargc,
				    hargv);
				if (next != NULL)
					after = next;
				next = remote_queue(after, "%s", show);
				if (next != NULL)
					after = next;
			}
			goto done;
		}
	}

	s = session_find(name);
	if (s != NULL) {
		rhost = remote_session_option(s, "@remote_host");
		if (rhost == NULL) {
			cmdq_error(item, "duplicate session: %s", name);
			goto fail;
		}
		if (strcmp(rhost, host) != 0) {
			cmdq_error(item, "session %s is remote to %s", name,
			    rhost);
			goto fail;
		}
		free(rhost);
		rhost = NULL;
		options_set_string(s->options, "@remote_session", 0, "%s",
		    name);
		if (port != NULL)
			options_set_string(s->options, "@remote_port", 0, "%s",
			    port);
		options_set_number(s->options, "destroy-unattached", 0);

		wl = s->curw;
		if (wl == NULL)
			wl = RB_MIN(winlinks, &s->windows);
		if (wl == NULL) {
			cmdq_error(item, "session %s has no windows", name);
			goto fail;
		}
		wp = wl->window->active;
		if (wp == NULL) {
			cmdq_error(item, "session %s has no panes", name);
			goto fail;
		}
		dead = window_pane_exited(wp);
		options_set_number(wp->options, "remain-on-exit", 1);
		options_set_string(wp->options, "remain-on-exit-format", 0,
		    "disconnected from %s", host);

		if (!detached)
			show = remote_build_attach(name, args, inside);
		if (!detached && !force && !dead) {
			fargv = remote_build_helper_argv(host, port, ctl, hash,
			    remote, 1, &fargc);
			respawn = remote_build_respawn(wp, fargc, fargv);
			if (remote_start_check(item, s, host, ctl, respawn,
			    show) == 0) {
				respawn = NULL;
				show = NULL;
				retval = CMD_RETURN_WAIT;
			} else {
				next = remote_queue(after, "%s", respawn);
				if (next != NULL)
					after = next;
				next = remote_queue(after, "%s", show);
				if (next != NULL)
					after = next;
			}
		} else if (force || dead) {
			next = remote_queue_respawn(after, wp, hargc, hargv);
			if (next != NULL)
				after = next;
			if (show != NULL) {
				next = remote_queue(after, "%s", show);
				if (next != NULL)
					after = next;
			}
		}
		goto done;
	}

	ssh = cmd_stringify_argv(hargc, hargv);

	/* Inside tmux, create the SSH session in a full-size modal pane. */
	if (inside && !detached) {
		if (wp == NULL || wp->window == NULL) {
			cmdq_error(item, "no current pane");
			goto fail;
		}
		if (wp->window->modal != NULL) {
			cmdq_error(item, "window already has a modal pane");
			goto fail;
		}
		if ((value = args_get(args, 'n')) != NULL)
			title = format_single(item, value, c, NULL, NULL,
			    NULL);
		else
			xasprintf(&title, "remote %s:%s", host, name);
		escaped = args_escape(title);
		next = remote_queue(after,
		    "new-pane -O -x 100%% -y 100%% -X 0 -Y 0 -T %s "
		    "-t %%%u -- %s", escaped, wp->id, ssh);
		free(escaped);
		if (next == NULL)
			goto fail;
		after = next;

		data = xcalloc(1, sizeof *data);
		data->window = wp->window->id;
		data->host = xstrdup(host);
		data->name = xstrdup(name);
		if (port != NULL)
			data->port = xstrdup(port);
		after = cmdq_insert_after(after,
		    cmdq_get_callback(cmd_new_remote_session_done, data));
		goto done;
	}

	remote_cat(&nflags, &nlen, "");
	if (args_has(args, 'D'))
		remote_cat(&nflags, &nlen, " -D");
	if (args_has(args, 'X'))
		remote_cat(&nflags, &nlen, " -X");
	if (args_has(args, 'E'))
		remote_cat(&nflags, &nlen, " -E");
	if (args_has(args, 'f')) {
		tmp = args_escape(args_get(args, 'f'));
		remote_cat(&nflags, &nlen, " -f ");
		remote_cat(&nflags, &nlen, tmp);
		free(tmp);
	}
	escaped = args_escape(name);
	next = remote_queue(after, "new-session%s -s %s%s -- %s",
	    (detached || inside) ? " -d" : "", escaped, nflags, ssh);
	free(escaped);
	if (next == NULL)
		goto fail;
	after = next;

	data = xcalloc(1, sizeof *data);
	data->window = UINT_MAX;
	data->host = xstrdup(host);
	data->name = xstrdup(name);
	if (port != NULL)
		data->port = xstrdup(port);
	after = cmdq_insert_after(after,
	    cmdq_get_callback(cmd_new_remote_session_done, data));

done:
	free(host);
	free(port);
	free(name);
	free(hash);
	free(ctl);
	free(remote);
	free(ssh);
	free(nflags);
	free(rhost);
	free(title);
	free(respawn);
	free(show);
	cmd_free_argv(argc, argv);
	cmd_free_argv(rargc, rargv);
	cmd_free_argv(hargc, hargv);
	cmd_free_argv(fargc, fargv);
	return (retval);

fail:
	free(host);
	free(port);
	free(name);
	free(hash);
	free(ctl);
	free(remote);
	free(ssh);
	free(nflags);
	free(rhost);
	free(title);
	free(respawn);
	free(show);
	cmd_free_argv(fargc, fargv);
	cmd_free_argv(argc, argv);
	cmd_free_argv(rargc, rargv);
	cmd_free_argv(hargc, hargv);
	return (CMD_RETURN_ERROR);
}

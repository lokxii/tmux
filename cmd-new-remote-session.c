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
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
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
 * Arguments: host port control-path token remote-command force key
 */
static const char remote_helper[] =
	"set -e\n"
	"host=$1; port=$2; ctl=$3; token=$4; cmd=$5; force=$6; key=$7\n"
	"mkdir -p \"${ctl%/*}\" 2>/dev/null || :\n"
	"chmod 700 \"${ctl%/*}\" 2>/dev/null || :\n"
	"set -- -o ControlMaster=auto -o ControlPath=\"$ctl\" "
	"-o ControlPersist=yes -o ServerAliveInterval=15 "
	"-o ServerAliveCountMax=3 -o TCPKeepAlive=yes "
	"-o ConnectTimeout=10\n"
	"[ -n \"$port\" ] && set -- \"$@\" -p \"$port\"\n"
	"[ -n \"$key\" ] && set -- \"$@\" -i \"$key\"\n"
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
	"\"mkdir -p \\\"\\$HOME/.cache/tmux-remote/$token\\\" && "
	"tar -C \\\"\\$HOME/.cache/tmux-remote/$token\\\" -xf -\" "
	"2>/dev/null || :\n"
	"fi\n"
	"exec ssh -tt \"$@\" \"$host\" \"$cmd\"\n";

/*
 * The remote side of the helper. It chooses the synced configuration (if
 * any), rewrites local home references to the cache directory, and runs the
 * requested tmux new-session command with any remaining arguments.
 *
 * Arguments from sh(1): token tmux-arguments...
 */
static const char remote_bootstrap[] =
	"token=$1\n"
	"shift\n"
	"d=\"$HOME/.cache/tmux-remote/$token\"\n"
	"c=\"$d/.tmux.conf\"\n"
	"[ -f \"$c\" ] || c=\"$d/.config/tmux/tmux.conf\"\n"
	"de=$(printf '%s' \"$d\" | sed 's/[\\\\&|]/\\\\&/g')\n"
	"if [ -f \"$c\" ]; then\n"
	"  sed -e 's|~/.tmux|'\"$de\"'/.tmux|g' "
	"-e 's|\\$HOME/.tmux|'\"$de\"'/.tmux|g' "
	"-e 's|~/.config/tmux|'\"$de\"'/.config/tmux|g' "
	"\"$c\" > \"$c.tmp\" && mv \"$c.tmp\" \"$c\"\n"
	"  export TMUX_PLUGIN_MANAGER_PATH=\"$d/.tmux/plugins\"\n"
	"  export TERM=xterm-256color\n"
	"  exec tmux -f \"$c\" \"$@\"\n"
	"fi\n"
	"export TERM=xterm-256color\n"
	"exec tmux \"$@\"\n";

/*
 * Server-side registry for remote sessions. The random token is chosen by
 * the server, not derived from user-controlled names, and the pane/session
 * ids are recorded only after this command creates them. Pane options alone
 * are not enough to reuse a pane.
 */
struct remote_entry {
	char			*host;
	char			*port;
	char			*name;
	char			*token;

	u_int			 session_id;

	TAILQ_ENTRY(remote_entry) entry;
};
static TAILQ_HEAD(remote_entries, remote_entry) remote_entries =
    TAILQ_HEAD_INITIALIZER(remote_entries);

struct cmd_new_remote_session_data {
	struct remote_entry	*entry;
	char			*host;
	char			*name;
	char			*port;
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

	.args = { "Ac:dDe:EF:f:H:i:n:Pp:Rs:t:x:Xy:", 0, -1, NULL },
	.usage = "[-AdDEPRX] [-c start-directory] [-e environment] "
		 "[-F format] [-f flags] [-H host] [-i identity-file] "
		 "[-n window-name] [-p port] [-s session-name] "
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

/* Generate a random token for control sockets and remote cache paths. */
static int
remote_random_token(char **out)
{
	static const char	 hexdigits[] = "0123456789abcdef";
	unsigned char		 buf[12];
	char			 hex[sizeof buf * 2 + 1];
	int			 fd;
	size_t			 off = 0, i;
	ssize_t			 n;

	fd = open("/dev/urandom", O_RDONLY);
	if (fd == -1)
		return (-1);
	while (off < sizeof buf) {
		n = read(fd, buf + off, sizeof buf - off);
		if (n == -1) {
			if (errno == EINTR)
				continue;
			close(fd);
			return (-1);
		}
		if (n == 0) {
			close(fd);
			errno = EIO;
			return (-1);
		}
		off += (size_t)n;
	}
	close(fd);

	for (i = 0; i < sizeof buf; i++) {
		hex[i * 2] = hexdigits[buf[i] >> 4];
		hex[i * 2 + 1] = hexdigits[buf[i] & 0xf];
	}
	hex[sizeof hex - 1] = '\0';
	*out = xstrdup(hex);
	return (0);
}

/* Find the registry entry for a host, port and remote session name. */
static struct remote_entry *
remote_entry_find(const char *host, const char *port, const char *name)
{
	const char		*p = port == NULL ? "" : port;
	struct remote_entry	*entry;

	TAILQ_FOREACH(entry, &remote_entries, entry) {
		if (strcmp(entry->host, host) != 0 ||
		    strcmp(entry->port, p) != 0 ||
		    strcmp(entry->name, name) != 0)
			continue;
		return (entry);
	}
	return (NULL);
}

/* Get or create the registry entry for a remote session. */
static struct remote_entry *
remote_entry_get(const char *host, const char *port, const char *name,
    char **cause)
{
	struct remote_entry	*entry;
	char			*token;

	if ((entry = remote_entry_find(host, port, name)) != NULL)
		return (entry);

	if (remote_random_token(&token) != 0) {
		xasprintf(cause, "cannot generate random token: %s",
		    strerror(errno));
		return (NULL);
	}
	entry = xcalloc(1, sizeof *entry);
	entry->host = xstrdup(host);
	entry->port = xstrdup(port == NULL ? "" : port);
	entry->name = xstrdup(name);
	entry->token = token;
	entry->session_id = UINT_MAX;
	TAILQ_INSERT_TAIL(&remote_entries, entry, entry);
	return (entry);
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

/* Is this pane running the remote helper? */
static int
remote_pane_is_helper(struct window_pane *wp)
{
	if (wp->argc < 3 || wp->argv == NULL)
		return (0);
	return (strcmp(wp->argv[0], "sh") == 0 &&
	    strcmp(wp->argv[1], "-c") == 0 &&
	    strcmp(wp->argv[2], remote_helper) == 0);
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
remote_build_command(const char *token, int argc, char **argv)
{
	char	*out = NULL, *quoted;
	size_t	 len = 0;
	int	 i;

	remote_cat(&out, &len, "sh -c ");
	quoted = remote_sh_quote(remote_bootstrap);
	remote_cat(&out, &len, quoted);
	free(quoted);
	remote_cat(&out, &len, " sh ");
	quoted = remote_sh_quote(token);
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
    const char *token, const char *command, int force, const char *key,
    int *argc)
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
	cmd_append_argv(argc, &argv, token);
	cmd_append_argv(argc, &argv, command);
	cmd_append_argv(argc, &argv, force ? "1" : "0");
	cmd_append_argv(argc, &argv, key == NULL ? "" : key);
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

/*
 * Make the local proxy session transparent: the remote tmux owns the status
 * line and the prefix key, while C-g is the host escape into a small local
 * key table.
 */
static void
remote_apply_local_options(struct session *s)
{
	options_set_number(s->options, "status", 0);
	options_set_number(s->options, "prefix", KEYC_NONE);
	options_set_number(s->options, "prefix2", KEYC_NONE);
	options_set_number(s->options, "destroy-unattached", 0);
}

static struct cmdq_item *
remote_install_bindings(struct cmdq_item *after)
{
	static int		 installed;
	struct cmdq_item	*next;

	if (installed)
		return (after);
	next = remote_queue(after,
	    "bind-key -n C-g switch-client -T tmux-remote-host ; "
	    "bind-key -T tmux-remote-host c new-session ; "
	    "bind-key -T tmux-remote-host d detach-client ; "
	    "bind-key -T tmux-remote-host s choose-tree -s ; "
	    "bind-key -T tmux-remote-host : command-prompt -P ; "
	    "bind-key -T tmux-remote-host ? list-keys -T tmux-remote-host ; "
	    "bind-key -T tmux-remote-host L switch-client -l ; "
	    "bind-key -T tmux-remote-host n switch-client -n ; "
	    "bind-key -T tmux-remote-host p switch-client -p");
	if (next == NULL)
		return (after);
	installed = 1;
	return (next);
}

/* Mark a pane as handling a remote session. */
static int
remote_mark_pane(struct window_pane *wp, struct cmd_new_remote_session_data *d)
{
	if (!remote_pane_is_helper(wp))
		return (0);
	options_set_string(wp->options, "@remote_host", 0, "%s", d->host);
	options_set_string(wp->options, "@remote_session", 0, "%s", d->name);
	if (d->port != NULL)
		options_set_string(wp->options, "@remote_port", 0, "%s",
		    d->port);
	return (1);
}

/* Set the metadata and lifetime options once the new session exists. */
static enum cmd_retval
cmd_new_remote_session_done(__unused struct cmdq_item *item, void *data)
{
	struct cmd_new_remote_session_data	*d = data;
	struct session				*s;
	struct winlink				*wl;
	struct window_pane			*wp;

	s = session_find(d->name);
	if (s != NULL) {
		options_set_string(s->options, "@remote_host", 0, "%s",
		    d->host);
		options_set_string(s->options, "@remote_session", 0, "%s",
		    d->name);
		if (d->port != NULL)
			options_set_string(s->options, "@remote_port", 0, "%s",
			    d->port);
		remote_apply_local_options(s);
		wl = s->curw;
		if (wl != NULL && (wp = wl->window->active) != NULL) {
			if (remote_mark_pane(wp, d))
				d->entry->session_id = s->id;
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
	struct window_pane		*wp;
	struct environ_entry		*ee;
	struct remote_entry		*entry;
	struct cmd_new_remote_session_data *data;
	const char			*value, *errstr;
	char				*host = NULL, *port = NULL, *key = NULL;
	char				*ename, *name = NULL, *token = NULL;
	char				*cause = NULL;
	char				*ctl = NULL;
	const char			*runtime;
	char				*remote = NULL, *ssh = NULL;
	char				*nflags = NULL, *rhost = NULL;
	char				*escaped, *tmp;
	char				*respawn = NULL, *show = NULL;
	size_t				 nlen = 0;
	char				**argv = NULL, **rargv = NULL, **hargv = NULL;
	char				**fargv = NULL;
	int				 argc = 0, rargc = 0, hargc = 0;
	int				 fargc = 0;
	int				 offset = 0, inside = 0, force, detached;
	int				 dead = 0;
	long long			 portnum;
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
		portnum = strtonum(port, 1, 65535, &errstr);
		if (errstr != NULL) {
			cmdq_error(item, "port %s", errstr);
			goto fail;
		}
		free(port);
		xasprintf(&port, "%lld", portnum);
	}
	if ((value = args_get(args, 'i')) != NULL) {
		key = format_single(item, value, c, NULL, NULL, NULL);
		if (*key == '\0' || strpbrk(key, "\t\r\n") != NULL) {
			cmdq_error(item, "invalid identity file: %s", key);
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

	entry = remote_entry_get(host, port, name, &cause);
	if (entry == NULL) {
		cmdq_error(item, "%s", cause);
		free(cause);
		goto fail;
	}
	token = entry->token;
	runtime = getenv("XDG_RUNTIME_DIR");
	if (runtime == NULL || *runtime == '\0')
		runtime = "/tmp";
	xasprintf(&ctl, "%s/tmux-remote-%u/%s.sock", runtime,
	    (u_int)getuid(), token);

	remote_build_remote_argv(args, name, offset, argc, argv, &rargc,
	    &rargv);
	remote = remote_build_command(token, rargc, rargv);
	hargv = remote_build_helper_argv(host, port, ctl, token, remote, force,
	    key, &hargc);
	after = remote_install_bindings(after);

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
		if (entry->session_id != s->id) {
			cmdq_error(item, "session %s is not owned by this "
			    "remote connection", name);
			goto fail;
		}
		options_set_string(s->options, "@remote_session", 0, "%s",
		    name);
		if (port != NULL)
			options_set_string(s->options, "@remote_port", 0, "%s",
			    port);
		remote_apply_local_options(s);

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

		if (!detached)
			show = remote_build_attach(name, args, inside);
		if (!detached && !force && !dead) {
			fargv = remote_build_helper_argv(host, port, ctl, token,
			    remote, 1, key, &fargc);
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
	data->entry = entry;
	data->host = xstrdup(host);
	data->name = xstrdup(name);
	if (port != NULL)
		data->port = xstrdup(port);
	after = cmdq_insert_after(after,
	    cmdq_get_callback(cmd_new_remote_session_done, data));

	if (inside && !detached) {
		escaped = remote_build_attach(name, args, 1);
		next = remote_queue(after, "%s", escaped);
		free(escaped);
		if (next != NULL)
			after = next;
	}

done:
	free(host);
	free(port);
	free(key);
	free(name);
	free(ctl);
	free(remote);
	free(ssh);
	free(nflags);
	free(rhost);
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
	free(key);
	free(name);
	free(ctl);
	free(remote);
	free(ssh);
	free(nflags);
	free(rhost);
	free(respawn);
	free(show);
	cmd_free_argv(fargc, fargv);
	cmd_free_argv(argc, argv);
	cmd_free_argv(rargc, rargv);
	cmd_free_argv(hargc, hargv);
	return (CMD_RETURN_ERROR);
}

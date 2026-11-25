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
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE OF DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>

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
 * Create a local mirror of a remote tmux session using control mode.
 *
 * The local server runs a helper which ssh's to the remote host, copies the
 * local tmux configuration to a private cache, starts a private remote tmux
 * server on a random socket, and finally execs `tmux -C attach`. The
 * control-mode stream is parsed by remote.c and mirrored into local windows
 * and panes.
 */

static const char remote_control_helper[] =
	"set -e\n"
	"host=$1; port=$2; ctl=$3; token=$4; key=$5; cmd=$6\n"
	"mkdir -p \"${ctl%/*}\" 2>/dev/null || :\n"
	"chmod 700 \"${ctl%/*}\" 2>/dev/null || :\n"
	"set -- -o ControlMaster=auto -o ControlPath=\"$ctl\" "
	"-o ControlPersist=yes -o ServerAliveInterval=15 "
	"-o ServerAliveCountMax=3 -o TCPKeepAlive=yes "
	"-o ConnectTimeout=10\n"
	"[ -n \"$port\" ] && set -- \"$@\" -p \"$port\"\n"
	"[ -n \"$key\" ] && set -- \"$@\" -i \"$key\"\n"
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
	"exec ssh -T \"$@\" \"$host\" \"$cmd\"\n";

/*
 * Remote side: select the copied config, create the private server/session if
 * needed, then attach in control mode.
 *
 * Arguments: token session-name new-session-arguments...
 */
static const char remote_control_bootstrap[] =
	"token=$1; name=$2; shift 2\n"
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
	"  if ! tmux -L \"$token\" has-session -t \"$name\" 2>/dev/null; "
	"then\n"
	"    tmux -L \"$token\" -f \"$c\" new-session -d -s \"$name\" "
	"\"$@\"\n"
	"  fi\n"
	"else\n"
	"  if ! tmux -L \"$token\" has-session -t \"$name\" 2>/dev/null; "
	"then\n"
	"    tmux -L \"$token\" new-session -d -s \"$name\" \"$@\"\n"
	"  fi\n"
	"fi\n"
	"export TERM=xterm-256color\n"
	"exec tmux -L \"$token\" -C attach -t \"$name\"\n";

struct remote_entry {
	char			*host;
	char			*port;
	char			*name;
	char			*token;

	TAILQ_ENTRY(remote_entry) entry;
};
static TAILQ_HEAD(remote_entries, remote_entry) remote_entries =
    TAILQ_HEAD_INITIALIZER(remote_entries);

static enum cmd_retval	cmd_new_remote_session_exec(struct cmd *,
			    struct cmdq_item *);

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

/* Generate a random token for the remote socket and cache path. */
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

/* Get or create a stable token for a host/port/name. */
static struct remote_entry *
remote_entry_get(const char *host, const char *port, const char *name,
    char **cause)
{
	struct remote_entry	*entry;
	char			*token;
	const char		*p = port == NULL ? "" : port;

	TAILQ_FOREACH(entry, &remote_entries, entry) {
		if (strcmp(entry->host, host) == 0 &&
		    strcmp(entry->port, p) == 0 &&
		    strcmp(entry->name, name) == 0)
			return (entry);
	}
	if (remote_random_token(&token) != 0) {
		xasprintf(cause, "cannot generate random token: %s",
		    strerror(errno));
		return (NULL);
	}
	entry = xcalloc(1, sizeof *entry);
	entry->host = xstrdup(host);
	entry->port = xstrdup(p);
	entry->name = xstrdup(name);
	entry->token = token;
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
		/*
		 * Do not keep '.' (or ':') in session names: tmux treats them
		 * as window/pane separators in -t targets, including the -t to
		 * attach-session.
		 */
		if (isalnum((unsigned char)*p) || *p == '-' || *p == '_')
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
 * Build new-session arguments for the private remote server. The remote
 * session is created detached and then attached with control mode.
 */
static void
remote_build_create_argv(struct args *args, int offset, int argc, char **argv,
    int *out_argc, char ***out_argv)
{
	struct args_value	*av;
	int			 i, n = 0;
	char		       **v = NULL;

	if (args_has(args, 'c'))
		remote_append_option(&n, &v, "-c", args_get(args, 'c'));
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

/* Build the remote control command string. */
static char *
remote_build_command(const char *token, const char *name, int argc, char **argv)
{
	char	*out = NULL, *quoted;
	size_t	 len = 0;
	int	 i;

	remote_cat(&out, &len, "sh -c ");
	quoted = remote_sh_quote(remote_control_bootstrap);
	remote_cat(&out, &len, quoted);
	free(quoted);
	remote_cat(&out, &len, " sh ");
	quoted = remote_sh_quote(token);
	remote_cat(&out, &len, quoted);
	free(quoted);
	remote_cat(&out, &len, " ");
	quoted = remote_sh_quote(name);
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

/* Build the local helper command string. */
static char *
remote_build_helper(const char *host, const char *port, const char *ctl,
    const char *token, const char *key, const char *command)
{
	char	*out = NULL, *quoted;
	size_t	 len = 0;

	remote_cat(&out, &len, "sh -c ");
	quoted = remote_sh_quote(remote_control_helper);
	remote_cat(&out, &len, quoted);
	free(quoted);
	remote_cat(&out, &len, " sh ");
	quoted = remote_sh_quote(host);
	remote_cat(&out, &len, quoted);
	free(quoted);
	remote_cat(&out, &len, " ");
	quoted = remote_sh_quote(port == NULL ? "" : port);
	remote_cat(&out, &len, quoted);
	free(quoted);
	remote_cat(&out, &len, " ");
	quoted = remote_sh_quote(ctl);
	remote_cat(&out, &len, quoted);
	free(quoted);
	remote_cat(&out, &len, " ");
	quoted = remote_sh_quote(token);
	remote_cat(&out, &len, quoted);
	free(quoted);
	remote_cat(&out, &len, " ");
	quoted = remote_sh_quote(key == NULL ? "" : key);
	remote_cat(&out, &len, quoted);
	free(quoted);
	remote_cat(&out, &len, " ");
	quoted = remote_sh_quote(command);
	remote_cat(&out, &len, quoted);
	free(quoted);
	return (out);
}

/* Get a string user option from a session, or NULL if not set. */
static char *
remote_session_option(struct session *s, const char *name)
{
	struct options_entry	*o;

	o = options_get_only(s->options, name);
	if (o == NULL)
		return (NULL);
	return (options_to_string(o, NULL, 0));
}

/* Queue a single command after the current command. */
static int
remote_queue(struct cmdq_item *item, const char *fmt, ...)
{
	va_list		 ap;
	struct cmd_parse_result *pr;
	struct cmdq_item	*new_item;
	char		*line;

	va_start(ap, fmt);
	xvasprintf(&line, fmt, ap);
	va_end(ap);

	pr = cmd_parse_from_string(line, NULL);
	free(line);
	if (pr->status != CMD_PARSE_SUCCESS) {
		cmdq_error(item, "%s",
		    pr->error == NULL ? "parse error" : pr->error);
		free(pr->error);
		return (-1);
	}
	new_item = cmdq_get_command(pr->cmdlist, NULL);
	cmd_list_free(pr->cmdlist);
	cmdq_insert_after(item, new_item);
	return (0);
}

static enum cmd_retval
cmd_new_remote_session_exec(struct cmd *self, struct cmdq_item *item)
{
	struct args		*args = cmd_get_args(self);
	struct cmd_find_state	 fs;
	struct client		*c = cmdq_get_client(item);
	struct session		*s, *loop;
	struct window_pane	*wp;
	struct environ_entry	*ee;
	struct remote_entry	*entry;
	const char		*value, *errstr;
	char			*host = NULL, *port = NULL, *key = NULL;
	char			*ename, *name = NULL, *token, *ctl = NULL;
	char			*cause = NULL;
	const char		*runtime;
	char			*command = NULL, *helper = NULL;
	char			*escaped, *tmp, *rhost = NULL, *p;
	char			*rname = NULL, *rport = NULL;
	char			**argv = NULL, **rargv = NULL;
	int			 argc = 0, rargc = 0;
	int			 offset = 0, inside = 0, force, detached;
	long long		 portnum;
	enum cmd_retval		 retval = CMD_RETURN_NORMAL;

	force = args_has(args, 'R');
	detached = args_has(args, 'd');
	(void)force;

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
		/*
		 * Avoid '.' and ':' in the session name: tmux treats them as
		 * window/pane separators in -t targets, so a dotted name (for
		 * example an IP address) cannot be attached by name.
		 */
		for (p = name; *p != '\0'; p++) {
			if (*p == '.' || *p == ':')
				*p = '_';
		}
	} else
		name = remote_default_name(host);

	/*
	 * Detect whether the command was run from a pane of this tmux server.
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

	/*
	 * Look for an existing mirror of the requested remote session by
	 * identity (host, remote session name and port) rather than by
	 * local name: the requested name may be held by an unrelated plain
	 * local session, and the mirror itself may have been renamed (for
	 * example to name-1) if a plain session held the name when it was
	 * created. Mirrors record their identity in the @remote_host,
	 * @remote_session and @remote_port options.
	 */
	s = NULL;
	RB_FOREACH(loop, sessions, &sessions) {
		rhost = remote_session_option(loop, "@remote_host");
		if (rhost == NULL)
			continue;
		rname = remote_session_option(loop, "@remote_session");
		rport = remote_session_option(loop, "@remote_port");

		/*
		 * Compare ports with an unset port treated as the ssh
		 * default of 22 on both sides: a mirror created without -p
		 * and a command run with -p 22 (or vice versa) are the same
		 * remote, while an explicit different port is not. Ports
		 * are normalized decimal strings on both sides, so a plain
		 * strcmp is enough.
		 */
		if (rname != NULL && strcmp(rhost, host) == 0 &&
		    strcmp(rname, name) == 0 &&
		    strcmp(rport == NULL ? "22" : rport,
		    port == NULL ? "22" : port) == 0) {
			s = loop;
		} else if (strcmp(loop->name, name) == 0) {
			/*
			 * A mirror holding the requested local name belongs
			 * to a different remote: refuse rather than
			 * silently reusing or duplicating it.
			 */
			cmdq_error(item, "session %s is remote to %s port %s",
			    name, rhost, rport == NULL ? "22" : rport);
			free(rhost);
			free(rname);
			free(rport);
			rhost = rname = rport = NULL;
			goto fail;
		}
		free(rhost);
		free(rname);
		free(rport);
		rhost = rname = rport = NULL;
		if (s != NULL)
			break;
	}
	if (s != NULL) {
		if (!detached) {
			remote_queue(item, inside ? "switch-client -t $%u" :
			    "attach-session -t $%u", s->id);
		}
		goto done;
	}

	remote_build_create_argv(args, offset, argc, argv, &rargc, &rargv);
	command = remote_build_command(token, name, rargc, rargv);
	helper = remote_build_helper(host, port, ctl, token, key, command);
	if (remote_start(helper, name, host, port, token,
	    c != NULL ? item : NULL, c, inside, detached) == NULL) {
		cmdq_error(item, "cannot start remote session");
		goto fail;
	}
	if (c != NULL)
		retval = CMD_RETURN_WAIT;

done:
	free(host);
	free(port);
	free(key);
	free(name);
	free(ctl);
	free(command);
	free(helper);
	free(rhost);
	free(rname);
	free(rport);
	cmd_free_argv(argc, argv);
	cmd_free_argv(rargc, rargv);
	return (retval);

fail:
	free(host);
	free(port);
	free(key);
	free(name);
	free(ctl);
	free(command);
	free(helper);
	free(rhost);
	free(rname);
	free(rport);
	cmd_free_argv(argc, argv);
	cmd_free_argv(rargc, rargv);
	return (CMD_RETURN_ERROR);
}

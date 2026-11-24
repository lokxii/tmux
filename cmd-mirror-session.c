/* $OpenBSD$ */

/*
 * Experimental command to start a control-mode mirror in the local server.
 *
 * The argument is a shell command that runs a remote "tmux -C attach"
 * (usually through ssh).  This is only a prototype entry point; it is not
 * wired into new-remote-session yet.
 */

#include <sys/types.h>

#include <stdlib.h>
#include <string.h>

#include "tmux.h"

static enum cmd_retval	cmd_mirror_session_exec(struct cmd *,
			    struct cmdq_item *);

const struct cmd_entry cmd_mirror_session_entry = {
	.name = "mirror-session",
	.description = "Mirror a remote control-mode session.",
	.alias = "mirror",

	.args = { "s:", 1, -1, NULL },
	.usage = "[-s session-name] command [argument ...]",

	.flags = CMD_STARTSERVER,
	.exec = cmd_mirror_session_exec
};

static enum cmd_retval
cmd_mirror_session_exec(__unused struct cmd *self, struct cmdq_item *item)
{
	struct args	*args = cmd_get_args(self);
	const char	*name = args_get(args, 's');
	char		*command = NULL, *tmp;
	size_t		 len = 0;
	u_int		 i;

	for (i = 0; i < args_count(args); i++) {
		const char *arg = args_string(args, i);
		size_t	    n;

		if (arg == NULL)
			continue;
		n = strlen(arg);
		tmp = xrealloc(command, len + n + 2);
		command = tmp;
		if (len != 0)
			command[len++] = ' ';
		memcpy(command + len, arg, n);
		len += n;
		command[len] = '\0';
	}
	if (command == NULL) {
		cmdq_error(item, "no remote command");
		return (CMD_RETURN_ERROR);
	}

	if (remote_start(command, name, NULL, NULL, NULL, NULL, NULL, 0, 0) == NULL) {
		cmdq_error(item, "cannot start remote: %s", command);
		free(command);
		return (CMD_RETURN_ERROR);
	}
	free(command);
	return (CMD_RETURN_NORMAL);
}

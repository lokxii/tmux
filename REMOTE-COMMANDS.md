# Remote-session command routing

`new-remote-session` is a control-mode mirror: the local tmux server owns the
local UI and mirrored windows/panes, while a private remote tmux server owns
the authoritative remote sessions, windows and panes.

The mirror runs a private remote tmux server with the copied local config. The
local tmux server is the only UI and owns the status line, key bindings and
command prompt. Remote windows and panes are mirrored into local windows and
panes, so there is no nested status bar and no separate remote leader key.

Routing is by command and target:

- commands that operate on sessions, clients or the local server run locally
  (`new-session`, `attach-session`, `detach-client`, `switch-client`,
  `list-sessions`, local options/keys, and so on);
- commands that operate on windows, panes or layouts are sent over the control
  channel to the remote tmux (`new-window`, `split-window`, `select-*`,
  `resize-*`, `swap-*`, layouts, copy-mode, and so on);
- commands with explicit targets are translated from local mirror ids to
  remote ids; unsupported targeted forms are rejected rather than silently
  changing only the local mirror.

The local prefix is the normal tmux prefix. There is no `C-g` or other host
escape key.

## Command classification

`H` = local/host tmux. `R` = remote tmux. `B` = both; use the corresponding
prefix or command prompt. For `B`, the command router decides from the command and its target:
session/client commands run locally, while window/pane commands are sent to
the remote control channel.

| Command | Side | Notes |
|---|---|---|
| `attach-session` | H | attach the local mirror session |
| `bind-key` | H | local key table |
| `break-pane` | R | remote pane/window operation |
| `capture-pane` | R | capture remote pane contents |
| `choose-buffer` | B | remote paste buffers by default |
| `choose-client` | H | local clients |
| `choose-tree` | B | window commands go remote; session chooser is local |
| `clear-history` | R | remote pane history |
| `clear-prompt-history` | H | local prompt history |
| `clock-mode` | R | remote pane |
| `command-prompt` | B | local prompt; typed commands are routed by name |
| `confirm-before` | B | runs the following command in the same layer |
| `copy-mode` | R | remote pane |
| `customize-mode` | H | local options UI |
| `delete-buffer` | B | use the layer's buffer store |
| `detach-client` | H | detaches the local client |
| `display-menu` | B | menu on the selected layer |
| `display-message` | B | message on the selected layer |
| `display-panes` | R | remote pane numbers |
| `display-popup` | B | popup on the selected layer |
| `find-window` | R | remote windows |
| `has-session` | H | local mirror session check |
| `if-shell` | B | runs following command in the same layer |
| `join-pane` | R | remote pane operation |
| `kill-pane` | R | remote pane |
| `kill-server` | H | local server; local server |
| `kill-session` | B | local session |
| `kill-window` | R | remote window |
| `last-pane` | R | remote pane |
| `last-window` | R | remote window |
| `link-window` | R | remote window |
| `list-buffers` | B | use the layer's buffer store |
| `list-clients` | H | local clients |
| `list-commands` | B | whichever layer is queried |
| `list-keys` | B | whichever layer is queried |
| `list-panes` | R | remote panes |
| `list-sessions` | H | local mirror sessions |
| `list-windows` | R | remote windows |
| `load-buffer` | B | use the layer's buffer store |
| `lock-client` | H | local client |
| `lock-server` | H | local server |
| `lock-session` | H | local mirror session |
| `move-pane` | R | remote pane |
| `move-window` | R | remote window |
| `new-pane` | R | remote floating pane |
| `new-remote-session` | H | this command |
| `new-session` | H | local session |
| `new-window` | R | `C-b c` |
| `next-layout` | R | remote layout |
| `next-window` | R | remote window |
| `paste-buffer` | B | remote paste buffers by default |
| `pipe-pane` | R | remote pane |
| `previous-layout` | R | remote layout |
| `previous-window` | R | remote window |
| `refresh-client` | B | whichever client is targeted |
| `rename-session` | B | local mirror by default|
| `rename-window` | R | remote window |
| `resize-pane` | R | remote pane |
| `resize-window` | R | remote window |
| `respawn-pane` | R | remote pane |
| `respawn-window` | R | remote window |
| `rotate-window` | R | remote window |
| `run-shell` | B | command runs in the layer that executes it |
| `save-buffer` | B | use the layer's buffer store |
| `select-layout` | R | remote window |
| `select-pane` | R | remote pane |
| `select-window` | R | remote window |
| `send-keys` | R | remote pane |
| `send-prefix` | R | send the prefix to a program in the remote pane |
| `server-access` | H | local server |
| `set-buffer` | B | use the layer's buffer store |
| `set-environment` | H | local environment |
| `set-hook` | H | local hooks |
| `set-option` | H | local options |
| `set-window-option` | H | local window options |
| `show-buffer` | B | use the layer's buffer store |
| `show-environment` | H | local environment |
| `show-hooks` | H | local hooks |
| `show-messages` | H | local server messages |
| `show-options` | H | local options |
| `show-prompt-history` | H | local prompt history |
| `show-window-options` | H | local window options |
| `source-file` | H | local config |
| `split-window` | R | remote split |
| `start-server` | H | local server |
| `suspend-client` | H | local client |
| `swap-pane` | R | remote pane |
| `swap-window` | R | remote window |
| `switch-client` | H | local clients |
| `switch-mode` | R | remote pane mode |
| `unbind-key` | H | local key table |
| `unlink-window` | R | remote window |
| `wait-for` | B | whichever layer runs it |

## Design note

Commands execute in the local tmux server, with window/pane operations sent
over the remote control channel and their results mirrored back. The local
server is the only UI and owns the status line, prefix and command prompt.

# Remote-session command routing

`new-remote-session` is a pty proxy: the local tmux server owns the proxy
session and the SSH pane, while the remote tmux server owns the remote
sessions, windows and panes.

The proxy session is configured so that:

- the local status line is off, so the **remote tmux status line is the only
  status bar**;
- the local prefix and prefix2 are disabled, so keys such as `C-b` go to the
  remote tmux;
- `C-g` is the host escape and enters the `tmux-remote-host` key table.

Routing rule:

```text
remote tmux prefix (normally C-b)  -> remote tmux command
C-g                                -> local/host command
remote command prompt (C-b :)      -> command runs in remote tmux
host command prompt (C-g :)        -> command runs in local tmux
```

The command names overlap because both servers are tmux. A command is routed
by the layer whose prefix or command prompt is used.

## Host key table

| Key | Command |
|---|---|
| `C-g c` | `new-session` |
| `C-g d` | `detach-client` |
| `C-g s` | `choose-tree -s` (local sessions) |
| `C-g :` | local `command-prompt -P` |
| `C-g ?` | `list-keys -T tmux-remote-host` |
| `C-g L` | `switch-client -l` |
| `C-g n` | `switch-client -n` |
| `C-g p` | `switch-client -p` |

Other host commands can be run from the host command prompt:
`C-g :new-session`, `C-g :list-sessions`, `C-g :set-option`, etc.

All other normal key bindings are handled by the remote tmux. In particular:

- `C-b c` -> remote `new-window`
- `C-b %` / `C-b "` -> remote split
- `C-b n` / `C-b p` -> remote next/previous window
- `C-b d` -> remote detach (the SSH proxy then exits)
- `C-b :` -> remote command prompt

## Command classification

`H` = local/host tmux. `R` = remote tmux. `B` = both; use the corresponding
prefix or command prompt. For `B`, the normal key binding or key sequence
usually reaches the remote side; the host side is available through `C-g :`.

| Command | Side | Notes |
|---|---|---|
| `attach-session` | H | attach a local proxy session |
| `bind-key` | H | local key table; remote via `C-b :` |
| `break-pane` | R | remote pane/window operation |
| `capture-pane` | R | capture remote pane contents |
| `choose-buffer` | B | remote paste buffers by default |
| `choose-client` | H | local clients |
| `choose-tree` | B | remote windows by default; `C-g s` for local sessions |
| `clear-history` | R | remote pane history |
| `clear-prompt-history` | H | local prompt history |
| `clock-mode` | R | remote pane |
| `command-prompt` | B | `C-b :` remote, `C-g :` host |
| `confirm-before` | B | runs the following command in the same layer |
| `copy-mode` | R | remote pane |
| `customize-mode` | H | local options UI |
| `delete-buffer` | B | use the layer's buffer store |
| `detach-client` | H | `C-g d` detaches the local client; `C-b d` detaches the remote tmux |
| `display-menu` | B | menu on the selected layer |
| `display-message` | B | message on the selected layer |
| `display-panes` | R | remote pane numbers |
| `display-popup` | B | popup on the selected layer |
| `find-window` | R | remote windows |
| `has-session` | H | local proxy session check |
| `if-shell` | B | runs following command in the same layer |
| `join-pane` | R | remote pane operation |
| `kill-pane` | R | remote pane |
| `kill-server` | H | local server; remote via `C-b :` (dangerous) |
| `kill-session` | B | local proxy by default; use `C-b :` for remote |
| `kill-window` | R | remote window |
| `last-pane` | R | remote pane |
| `last-window` | R | remote window |
| `link-window` | R | remote window |
| `list-buffers` | B | use the layer's buffer store |
| `list-clients` | H | local clients |
| `list-commands` | B | whichever layer is queried |
| `list-keys` | B | whichever layer is queried |
| `list-panes` | R | remote panes |
| `list-sessions` | H | local proxy sessions; remote via `C-b :` |
| `list-windows` | R | remote windows |
| `load-buffer` | B | use the layer's buffer store |
| `lock-client` | H | local client |
| `lock-server` | H | local server |
| `lock-session` | H | local proxy session |
| `move-pane` | R | remote pane |
| `move-window` | R | remote window |
| `new-pane` | R | remote floating pane |
| `new-remote-session` | H | this command |
| `new-session` | H | `C-g c`; remote via `C-b :` |
| `new-window` | R | `C-b c` |
| `next-layout` | R | remote layout |
| `next-window` | R | remote window |
| `paste-buffer` | B | remote paste buffers by default |
| `pipe-pane` | R | remote pane |
| `previous-layout` | R | remote layout |
| `previous-window` | R | remote window |
| `refresh-client` | B | whichever client is targeted |
| `rename-session` | B | local proxy by default; remote via `C-b :` |
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
| `set-environment` | H | local environment; remote via `C-b :` |
| `set-hook` | H | local hooks; remote via `C-b :` |
| `set-option` | H | local options; remote via `C-b :` |
| `set-window-option` | H | local window options; remote via `C-b :` |
| `show-buffer` | B | use the layer's buffer store |
| `show-environment` | H | local environment; remote via `C-b :` |
| `show-hooks` | H | local hooks; remote via `C-b :` |
| `show-messages` | H | local server messages |
| `show-options` | H | local options; remote via `C-b :` |
| `show-prompt-history` | H | local prompt history |
| `show-window-options` | H | local window options |
| `source-file` | H | local config; remote via `C-b :` |
| `split-window` | R | remote split |
| `start-server` | H | local server |
| `suspend-client` | H | local client |
| `swap-pane` | R | remote pane |
| `swap-window` | R | remote window |
| `switch-client` | H | local clients; `C-g L/n/p` |
| `switch-mode` | R | remote pane mode |
| `unbind-key` | H | local key table; remote via `C-b :` |
| `unlink-window` | R | remote window |
| `wait-for` | B | whichever layer runs it |

## Design note

This routing keeps the local proxy session transparent without mirroring the
remote state into local windows. It is still a nested pty connection: the
remote tmux owns the visible status line and its prefix. A future control-mode
implementation could make the local server authoritative instead, but that is
a much larger change.

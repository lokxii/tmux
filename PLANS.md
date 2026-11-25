# PLANS.md — SSH-backed remote tmux sessions

## Status

The implementation uses a **control-mode mirror**:

- a private remote tmux server runs the copied local config/plugins;
- the local tmux server is the only UI;
- remote windows/panes are mirrored into local windows/panes;
- the local status line is the only status bar;
- commands are routed by name/target, with no separate leader key.

The earlier pty-proxy approach has been removed from `new-remote-session`.

## Architecture

```text
local tmux server
  ├─ local mirror session/windows/panes     <- local UI, status, key bindings
  └─ job: sh helper
       ├─ ssh ControlMaster
       ├─ copy local ~/.tmux, ~/.tmux.conf, ~/.config/tmux
       └─ ssh -T host
            └─ tmux -L <random-token> -f <copied-config> -C attach
                 └─ private remote tmux server
                      └─ authoritative remote windows/panes
```

The local server parses the control-mode protocol from the SSH child. Remote
`%output` is decoded and fed to local pane terminal parsers. Local input is
encoded and sent back with `send-keys -t %remote -H`. Window/layout/session
notifications update the local mirror.

The remote host needs only standard, non-forked tmux.

## Files

| File | Purpose |
|---|---|
| `cmd-new-remote-session.c` | `new-remote-session` command, argument parsing, random token registry, SSH/config-copy helper, private-socket bootstrap. |
| `remote.c` | Control-mode parser, bootstrap, remote/local ID maps, mirrored windows/panes, output/input, command routing. |
| `cmd-mirror-session.c` | Low-level `mirror-session` command used by the control-mode regression test. |
| `cmd-queue.c` | Pre-dispatch hook that routes window/pane commands to the remote control stream. |
| `window.c`, `tmux.h`, `server-client.c` | `PANE_REMOTE` liveness/key handling for mirror panes with `fd == -1`. |
| `REMOTE-COMMANDS.md` | Host/remote command classification. |
| `regress/control-mirror.sh` | Two-local-tmux-server control-mode test. |
| `regress/new-remote-session.sh` | End-to-end test using a fake ssh that runs the remote command locally. |

## Command

```
new-remote-session [-AdDEPRX] [-c start-directory] [-e environment]
                   [-F format] [-f flags] [-H host] [-i identity-file]
                   [-n window-name] [-p port] [-s session-name]
                   [-t target-session] [-x width] [-y height]
                   [user@host] [shell-command [argument ...]]
```

- Host comes from `-H` or the first positional argument.
- `-p` is the SSH port, `-i` the SSH identity file.
- `-R` forces a reconnect path and is accepted for compatibility.
- Session names are normalized: `.` and `:` become `_`, because tmux treats
  them as window/pane separators in `-t` targets (the default name derived
  from a host such as an IP address would otherwise be unattachable).
- With `-s` the remote session keeps the given name, while the local mirror
  session is named `<host>~<name>` with the host sanitized the same way (the
  part after any `@`, keeping alphanumerics, `-` and `_`). The prefix tells
  mirrors of the same remote session name on different hosts apart.
- Without `-s` the default `remote-<host>` name is used for both the remote
  and local session.
- Remote creation forwards `new-session` options (`-c`, `-e`, `-E`, `-F`,
  `-f`, `-n`, `-P`, `-t`, `-x`, `-X`, `-y`, and the shell command).
- `-d` starts the mirror without attaching/switching the client.

The remote session is created detached on a private tmux socket and then
attached with `tmux -C`. The random token is stable per host/port/name for the
lifetime of the local tmux server, so later invocations reuse the same remote
socket, cache, and SSH ControlMaster. An existing mirror is found by its
`@remote_host`/`@remote_session`/`@remote_port` identity, not by local session
name: a same-named plain local session does not hijack the attach, and
mirrors are not duplicated.

## Command routing

The local mirror is the UI, but commands are routed by semantics:

- **Local:** session/client/local-server commands (`new-session`,
  `attach-session`, `detach-client`, `switch-client`, `list-sessions`, local
  options/keys, etc.).
- **Remote:** window/pane/layout commands (`new-window`, `split-window`,
  `select-*`, `resize-*`, `swap-*`, `respawn-*`, layouts, etc.).
- Explicit local targets are translated to remote `@window` / `%pane` IDs for
  the supported targeted commands; `new-pane -t` is translated exactly like
  `split-window -t`, and `next-window`/`previous-window`/`last-window` with
  `-t` are translated to the remote session id. Unsupported targeted forms
  are rejected rather than silently desynchronizing the mirror; commands with
  `-s` source targets (`join-pane`, `link-window`, `move-window`,
  `move-pane`, `swap-pane`, `swap-window`) are rejected because source
  targets are not translated.

The attached client's terminal size is advertised to the remote with
`refresh-client -C` on attach and on resize, so the remote window follows the
local mirror window. Connecting and bootstrap time out after 60 seconds: a
stalled control connection fails the waiting `new-remote-session` command
instead of hanging forever.

There is no dedicated remote leader key. The normal tmux prefix and command
prompt work; for example `prefix c` creates a remote window and
`prefix :new-session` creates a local session. See `REMOTE-COMMANDS.md`.

## Bootstrap and mirroring

On `%session-changed` the mirror sends:

```text
list-windows -t $session -F ...
list-panes -s -t $session -F ...
```

Replies use a printable `|` field separator rather than tabs. Control clients
without a UTF-8 locale (the usual case for a non-interactive SSH session) have
command output passed through `utf8_sanitize()`, which turns every control
character, including tab, into `_`. Window names are the final field so they
may contain the separator.

Local windows are created with `window_create()`/`winlink_add()`; panes are
created with `window_add_pane()` and receive a `bufferevent_pair` for input.
A local window is created together with its first pane, and the local session
is only created once a window exists, so a mirror session always has a current
window. Remote layouts are applied with `layout_parse()`. Initial screen
contents are not yet seeded (see limitations); live `%output` is rendered.

## Input/output

- Remote `%output`: unvis-decode, then `input_parse_buffer()`.
- Local input: `input_key()` encodes using the mirror pane's screen modes;
  bytes are batched as `send-keys -t %remote -H <hex...>`.
- Local paste follows the same path through the pane event pair.

## SSH/config inheritance

The helper:

1. establishes a persistent SSH ControlMaster
   (`ControlPersist=yes`, keepalives, optional `-p`/`-i`);
2. copies local `~/.tmux`, `~/.tmux.conf`, `~/.config/tmux` to
   `~/.cache/tmux-remote/<token>/` on the remote;
3. rewrites path references with escaped `sed` replacements;
4. starts a private remote tmux server with the copied config and
   `TERM=xterm-256color`.

Because the remote server uses a private socket, the copied config is loaded
even when the remote user already has a default tmux server running.

## Attach, detach, exit

- Local detach leaves the control connection running.
- The ControlMaster remains available for a later invocation.
- Remote `%exit`/EOF frees the mirror; the local mirror session is destroyed
  through the safe destroy path, so attached clients are cleanly detached
  instead of the local server crashing.
- Killing the local mirror session stops the control connection and frees the
  mirror; the other local sessions are unaffected.
- A later invocation recreates the mirror and reuses the private remote
  server if it still exists.

## Testing

- `regress/control-mirror.sh` validates parser/bootstrap/output/input/layout/
  window events using two local tmux servers, no SSH.
- `regress/new-remote-session.sh` validates the full local command with a fake
  ssh that executes the remote control command locally.
- `regress/remote-ssh/run-mirror.sh` validates the same flow over real SSH,
  including config/plugin inheritance and command routing.
- ASAN runs of all three pass.

## Known limitations

- No initial screen seeding/scrollback; only output after control attach is
  replayed. `capture-pane` seeding is the next fidelity item.
- Mouse, images, OSC clipboard/passthrough, and `%extended-output` flow
  control are not implemented.
- Remote window index moves are not fully mirrored.
- Commands with `-s` source targets (`join-pane`, `link-window`,
  `move-window`, `move-pane`, `swap-pane`, `swap-window`) are rejected;
  source targets are not translated.
- The remote private tmux server, the copied configuration under
  `~/.cache/tmux-remote/<token>` on the remote host, and the local SSH
  ControlMaster persist after the mirror closes until explicitly killed, and
  the copied configuration may contain sensitive material. This is a
  documented trade-off of the private-server design.
- The remote mirror uses local tmux options/status; remote-only configuration
  differences are not mirrored back.

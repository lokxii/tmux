# PLANS.md — SSH-backed remote tmux sessions

## Goal

Add a first-class `new-remote-session` command which creates/attaches a tmux
session whose terminal is an SSH connection to a remote tmux server, while
keeping tmux's normal pty, resize, input, output, detach and redraw semantics.

## Summary of the chosen design

A remote session is a normal local tmux session (or, when invoked from inside
tmux, a new local session that the invoking client switches to) whose first
pane runs an SSH client. SSH allocates a remote pty and starts a remote tmux
client (`tmux new-session -A ...`) inside it.

This deliberately reuses the existing pane process path:

```
local tmux server
  └─ local session ("remote: host")           <- metadata/format
       └─ window
            └─ pane (pty)
                 └─ /bin/sh helper script
                      ├─ ssh (ControlMaster) copies local config/plugins
                      └─ exec ssh -tt host 'tmux -f <synced config> ...'
```

Input and output therefore use exactly the same code paths as any other pane:
`window_pane_key()` → pty master → ssh; ssh/remote tmux output → pty master →
`input_parse_buffer()` → local screen. This gives correct terminal emulation,
mouse, resize (`TIOCSWINSZ` → SSH window-change), true colour, alt-screen and
copy-mode without adding a second terminal parser or a control-mode client.

### Why not control-mode mirroring?

The existing upstream architecture has no remote session support. A previous
experimental fork (`snizovtsev/tmux`, branch `remote-draft-1`) mirrors remote
windows/panes by acting as a control-mode client. That is a much larger change
(~1,900 LOC against an older tree, with incomplete flow-control, layout,
mouse and reconnect handling). The objective explicitly asks for minimal,
maintainable changes where the current architecture can support the feature.
The pty-proxy design above supports all requested behaviour without rewriting
tmux's window/session model. Control-mode mirroring is left as a possible
future enhancement; the metadata and command surface below would not block it.

## Command

```
new-remote-session [-AdDEPRX] [-c start-directory] [-e environment]
                   [-F format] [-f flags] [-H host] [-n window-name]
                   [-p port] [-s session-name] [-t target-session]
                   [-x width] [-y height]
                   [user@host] [shell-command [argument ...]]
```

- **Host** comes from `-H host` or, if absent, the first positional argument.
  The remaining positional arguments are the remote `shell-command`.
- **`-p port`** is passed to `ssh -p`.
- **`-R`** forces a fresh SSH connection (kills the old pane and control
  master) before attaching.
- All `new-session` flags/options are accepted. Flags meaningful to the remote
  session (`-A -c -D -e -E -F -f -n -P -s -t -x -X -y` and the command) are
  forwarded to the remote `tmux new-session`. `-d` is local-only (the remote
  tmux must stay attached to its SSH pty so its output is visible).
- The command always uses remote `new-session -A -s NAME`, so an existing
  remote session is attached instead of failing.

### Inside vs outside tmux

- **Outside tmux (CLI):** create a local session and attach the invoking
  client. Input comes from the client's stdin; output goes to its stdout. This
  is the standard tmux client path.
- **Inside another tmux session:** the enclosing pane is detected via
  `TMUX_PANE`; a new local session is created detached and the current client
  switches to it (`switch-client`). The remote terminal therefore occupies the
  client's full session area above the status line instead of nesting a second
  tmux client inside a pane. `-d` suppresses the switch.

If a local session with the same name already exists and belongs to the same
remote host, the command attaches/switches to it instead of creating a
duplicate. If its pane has exited, or if `-R` was given, the pane is respawned
with a fresh SSH helper.

## Session metadata and formats

Remote identity is stored as session **user options**, so no session struct or
serialization changes are needed:

- `@remote_host` — SSH target (`user@host`, or `[user@]host`).
- `@remote_session` — remote tmux session name.
- `@remote_port` — SSH port, if non-default.

New formats (added in the sorted `format_table` in `format.c`):

- `#{session_remote}` — remote host or empty.
- `#{session_remote_host}` — alias for the host.
- `#{session_remote_session}` — remote session name.

`list-sessions`' default template gains
`[remote <host>:<session>]` when `@remote_host` is set, satisfying the
"indication that it is a remote session and the server domain" requirement.
Users can also put `#{session_remote}` in `status-left`/`pane-border-format`.

## SSH lifetime, attach and detach

The helper script runs SSH with:

- `ControlMaster=auto`, `ControlPath=<runtime dir>/tmux-remote-<uid>/<hash>.sock`,
  `ControlPersist=yes` — a persistent authenticated transport.
- `ServerAliveInterval=15`, `ServerAliveCountMax=3`, `TCPKeepAlive=yes` — a
  black-holed/hung connection terminates by itself instead of hanging forever.
- `ConnectTimeout=10`.

Behaviour:

- **Local detach** only detaches the local client. The server keeps the SSH
  child and pane alive, and keeps parsing its output while unattached; the
  remote tmux client remains attached to its pty. Reattach is instant.
- **Remote tmux detach** ends the remote tmux client, so the interactive SSH
  process exits, but the `ControlMaster` transport stays alive. The local pane
  is kept by `remain-on-exit` and the local session survives; a later attach
  respawns the SSH client, reusing the persistent transport (no new TCP/auth
  handshake).
- **Attach to an existing remote session:** if the local proxy session exists,
  attach/switch to it (respawning a dead pane). If only the remote session
  exists, remote `new-session -A` attaches to it.
- **Hung SSH:** keepalives terminate the child; reattach respawns it. `-R`
  explicitly kills the old control connection and reconnects immediately.

The helper sets `remain-on-exit on` and `destroy-unattached off` on the remote
session (via a completion callback after creation) so an SSH exit never
silently destroys the local proxy session.

## Config and plugin inheritance

The SSH pane runs a small POSIX `sh` helper before `exec`ing the interactive
SSH:

1. It copies the local `~/.tmux`, `~/.tmux.conf` and `~/.config/tmux` (when
   present) to a private cache directory on the remote
   (`~/.cache/tmux-remote/<hash>/`) over the same ControlMaster, using
   `tar | ssh 'tar -x'`.
2. On the remote side it rewrites `~/.tmux`, `$HOME/.tmux` and
   `~/.config/tmux` references in the copied config to the cache directory, so
   `~/.tmux/plugins/...`, TPM `run` lines and `source-file` lines keep
   working without touching the remote user's own files.
3. It exports `TMUX_PLUGIN_MANAGER_PATH` to the copied plugin directory and
   starts remote tmux with `-f <copied config>`. If no config was copied, the
   remote's own default config is used.
4. It forces `TERM=xterm-256color` for the remote tmux client so a remote host
   without `tmux-256color` terminfo can still run it.

The sync is best-effort: an unreachable host or missing `tar` falls through to
the interactive SSH attempt, and remote tmux still starts with its own config.
Because the copy lives in a cache directory, the remote user's files are never
modified.

## Files to change

| File | Change |
|---|---|
| `cmd-new-remote-session.c` (new) | Command entry, host/flag parsing, remote argv construction, helper script, attach/reconnect logic, completion callback. |
| `cmd.c` | Register the new command. |
| `Makefile.am` | Add the new source file. |
| `format.c` | Add `session_remote*` callbacks and table entries (sorted). |
| `cmd-list-sessions.c` | Show remote identity in the default session line. |
| `tmux.1` | Document the command, flags, formats, lifetime and config sync. |
| `PLANS.md` | This plan. |

No changes to the pane, session, client or input/output core are needed.

## Implementation sequence

1. Register command and format variables; wire `Makefile.am`.
2. Implement parsing and remote/local argument construction with strict
   validation (`host` cannot start with `-`, numeric port, safe session name).
3. Implement the local helper script (ControlMaster, keepalive, config/plugin
   sync, remote config rewrite, `exec ssh`).
4. Implement queued creation (`new-session`/`switch-client`/`attach-session`),
   duplicate/attach handling and respawn on dead pane/`-R`.
5. Add completion callback for `remain-on-exit`, `destroy-unattached` and
   `@remote_*` metadata.
6. Docs and regression/build verification.

## Test plan

- Build cleanly (`make`, warnings as errors are part of tmux's default flags).
- `tmux -L test list-commands` includes `new-remote-session`.
- Use a fake `ssh` in `PATH` that records argv, to verify without a remote:
  - session creation, remote `new-session -A` argument forwarding;
  - host/port/session-name validation;
  - `@remote_*` metadata and `#{session_remote*}` formats;
  - `list-sessions` default line shows `[remote host:session]`;
  - `remain-on-exit` keeps the proxy after the fake ssh exits;
  - second invocation attaches the existing local session and respawns a dead
    pane;
  - `-R` passes the force-reconnect path;
  - config/plugin copy invocation and `sh -n` syntax.
- Exercise attach/detach with the built tmux (no real SSH needed for the
  local-session lifecycle); if a local sshd is available, also test localhost.
- Re-run existing relevant regression tests if the harness can run them.

## Risks / limitations

- The remote tmux still owns its own key bindings/prefix. This is inherent to
  nested tmux; the pty proxy preserves both prefixes exactly as a manual
  `ssh -t host tmux ...` would.
- Copying plugin *scripts* is best-effort; configs that hard-code paths outside
  `~/.tmux` are not rewritten.
- `ControlPersist=yes` keeps a background ssh master after the local session is
  gone until it is killed or the user logs out. `-R` and `ssh -O exit` can be
  used to clean it up; a future session-closed hook can automate this.
- The remote host must have `tmux` and `sh`; missing binaries produce a normal
  pane death with the SSH error visible (pane remains due to
  `remain-on-exit`).

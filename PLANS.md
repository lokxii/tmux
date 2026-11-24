# PLANS.md — SSH-backed remote tmux sessions

## Goal

Add a first-class `new-remote-session` command which creates/attaches a tmux
session whose terminal is an SSH connection to a remote tmux server, while
keeping tmux's normal pty, resize, input, output, detach and redraw semantics.

## Summary of the chosen design

A remote session is a normal local tmux session whose pane runs an SSH client.
When invoked from inside tmux, a dedicated local proxy session is created and
the client switches to it. SSH allocates a remote pty and starts a remote tmux
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
                   [-F format] [-f flags] [-H host] [-i identity-file]
                   [-n window-name] [-p port] [-s session-name]
                   [-t target-session] [-x width] [-y height]
                   [user@host] [shell-command [argument ...]]
```

- **Host** comes from `-H host` or, if absent, the first positional argument.
  The remaining positional arguments are the remote `shell-command`.
- **`-p port`** is passed to `ssh -p`.
- **`-i identity-file`** is passed to `ssh -i` for both the configuration copy
  and the interactive session.
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
- **Inside another tmux session:** the enclosing pane/server is detected via
  `TMUX`/`TMUX_PANE`. A dedicated local proxy session is created detached and
  the invoking client switches to it, so the remote session fills the client
  and there is no second local status bar. Re-running the command attaches or
  switches to that same proxy session instead of creating another one. `-d`
  creates the detached session without switching.

The local proxy session is configured for transparent key routing:

- local `status` is off, so the remote tmux status line is the only one;
- local `prefix` and `prefix2` are disabled, so the remote tmux receives its
  own prefix (normally `C-b`);
- a root binding for `C-g` enters the `tmux-remote-host` key table, which
  provides host-side commands (`c` new-session, `d` detach-client, `s`
  choose-tree for local sessions, `:` local command prompt via
  `command-prompt -P`, `?` local keys, `L`/`n`/`p` local session switching).

Thus remote commands (`new-window`, splits, select/kill/resize/swap/copy-mode,
remote command prompt, and so on) use the remote prefix, while local proxy
management (`new-session`, detach, local command prompt, local sessions) uses
`C-g`. The full command classification is in `REMOTE-COMMANDS.md`.

If a local session with the same name already exists and belongs to the same
remote host, the command attaches to it instead of creating a duplicate. If
its pane has exited, or if `-R` was given, the pane is respawned with a fresh
SSH helper. Before attaching to an existing live remote pane, an asynchronous
`ssh -O check` probe plus a multiplexed `true` round trip (5 second timeout) runs; a failed or hung probe respawns
the pane with a forced reconnect (`ssh -O exit` first), then attaches.

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
- `#{pane_remote}`, `#{pane_remote_host}`, `#{pane_remote_session}` — the same
  identity for the local proxy pane.

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
- **Remote tmux detach or remote shell exit** ends the remote tmux client, so
  the interactive SSH process exits. With the default `remain-on-exit off` the
  SSH pane is killed normally; if it was the last pane, the local session and
  window are destroyed with it (standard tmux behavior). If `remain-on-exit`
  is configured on, the dead pane and session are kept normally. The
  `ControlMaster` transport stays alive, so a
  later invocation recreates the local session/pane and reuses the persistent
  transport (no new TCP/auth handshake).
- **Attach to an existing remote session:** if the local proxy session still
  exists, run an attach-time liveness check and attach to it. If only the
  remote session exists, remote `new-session -A` attaches to it.
- **Hung SSH:** keepalives terminate a black-holed connection by themselves.
  In addition, attaching a live remote pane runs an asynchronous
  `ssh -O check` plus multiplexed round trip with a 5 second timeout; a failed or hung probe kills
  the old pane/control connection and re-establishes it automatically before
  attaching. `-R` does the same immediately, without waiting for the probe.

The helper does not override `remain-on-exit`; its standard default is `off`,
so an SSH exit kills the pane, while users may set `on`, `key` or other values
normally. It sets `destroy-unattached off` on the local session so a local
client detach does not reap it.

Control socket and remote cache names use a server-generated 96-bit random
token rather than a hash of the public host/session names. A server-side
registry maps each `(host, port, name)` to that token and to the session id
created by this command. Session reuse requires the registered session id, the
helper argv and the pane metadata to match; forged pane options alone cannot
redirect a client to an attacker-chosen pane. The remote config rewrite also
escapes `&`, `|` and `\` in the cache path before using it as a `sed`
replacement.

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
| `format.c` | Add `session_remote*` and `pane_remote*` callbacks and table entries (sorted). |
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
4. Implement queued creation (`new-session`/attach), the inside-tmux proxy
   session and key/status routing, duplicate/attach handling, and respawn on
   dead pane/`-R`.
5. Add completion callbacks for `remain-on-exit`, `destroy-unattached` and
   `@remote_*` metadata.
6. Add the asynchronous attach-time `ssh -O check` probe with timeout and
   forced reconnect.
7. Docs and regression/build verification.

## Test plan

- Build cleanly (`make`, warnings as errors are part of tmux's default flags).
- `tmux -L test list-commands` includes `new-remote-session`.
- Use a fake `ssh` in `PATH` that records argv, to verify without a remote:
  - session creation, remote `new-session -A` argument forwarding;
  - host/port/session-name validation;
  - `@remote_*` metadata and `#{session_remote*}` formats;
  - `list-sessions` default line shows `[remote host:session]`;
  - `remain-on-exit` defaults off (not forced) and the SSH pane plus its
    last-pane session are killed when the fake ssh exits; enabling
    `remain-on-exit` keeps the pane and session;
  - a second invocation recreates the local session while reusing the SSH
    control connection;
  - `-R` passes the force-reconnect path;
  - config/plugin copy invocation and `sh -n` syntax;
  - inside-tmux creation/reuse of the dedicated proxy session, disabled local
    status/prefix and the `C-g` host key table, driven through a second tmux
    server acting as the attached pty client;
  - a hung `ssh -O check` probe triggers the forced kill and respawn.
- Exercise attach/detach with the built tmux (no real SSH needed for the
  local-session lifecycle); if a local sshd is available, also test localhost.
- Re-run existing relevant regression tests if the harness can run them.

## Risks / limitations

- This is still a pty proxy, not a control-mode mirror. The remote tmux owns
  its key bindings; the local status is hidden and the local prefix is disabled
  so the remote UI and prefix are used normally. Host management is reached
  through `C-g`. If the remote config changes its prefix to `C-g`, that key
  will conflict with the host escape.
- Copying plugin *scripts* is best-effort; configs that hard-code paths outside
  `~/.tmux` are not rewritten.
- `ControlPersist=yes` keeps a background ssh master after the local session is
  gone until it is killed or the user logs out. `-R` and `ssh -O exit` can be
  used to clean it up; a future session-closed hook can automate this.
- The remote host must have `tmux` and `sh`; missing binaries produce a normal
  pane death with the SSH error visible. Because the pane is not kept after
  exit, the local session disappears if it was the only pane.

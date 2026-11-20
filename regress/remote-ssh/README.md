# Real SSH integration tests for `new-remote-session`

`regress/remote-ssh/run.sh` exercises `tmux new-remote-session` (alias `newr`)
against a real SSH target. The machine running the script is the **local tmux
host**; `REMOTE_HOST` names the **remote SSH target** where the remote tmux
server runs.

The harness is a single POSIX `sh` script. It does not use Python, `expect`,
GNU-only tools, or root on either side.

## Quick start

Build tmux on the laptop first, then run from the top of the checkout:

```sh
cd /path/to/tmux
REMOTE_HOST=user@remote-host sh regress/remote-ssh/run.sh
```

`run.sh` is executable, so this is equivalent:

```sh
REMOTE_HOST=user@remote-host regress/remote-ssh/run.sh
```

Set up SSH authentication and accept the host key once before running:

```sh
ssh user@remote-host true
```

The harness probes with `BatchMode=yes`, so key/agent authentication must
already work. The actual interactive session uses ordinary `ssh` and is allowed
to prompt, but a password-only setup will not work well in the test ptys;
configure a key or agent first.

### Common options

```sh
# Non-default SSH port
REMOTE_HOST=user@remote-host REMOTE_PORT=2222 \
    sh regress/remote-ssh/run.sh

# Use a specific SSH private key (passed to ssh -i and the feature's -i)
REMOTE_HOST=user@remote-host SSH_IDENTITY=$HOME/.ssh/id_ed25519 \
    sh regress/remote-ssh/run.sh

# Use a specific local tmux binary
REMOTE_HOST=user@remote-host \
    TMUX_BIN=/path/to/tmux sh regress/remote-ssh/run.sh

# Keep local test files after a failure (or for inspection)
REMOTE_HOST=user@remote-host KEEP_TEST_ROOT=1 \
    sh regress/remote-ssh/run.sh

# Put the disposable test root under a chosen parent directory
REMOTE_HOST=user@remote-host LOCAL_TEST_ROOT=/tmp/my-tmux-tests \
    sh regress/remote-ssh/run.sh

# Local-only checks; no SSH connection is made
sh regress/remote-ssh/run.sh --check
sh regress/remote-ssh/run.sh --help
```

`TMUX_BIN` defaults to `<checkout>/tmux` when it is executable, otherwise to
`tmux` found in `PATH`. The binary must include `new-remote-session`; a plain
upstream `tmux` will fail the local prerequisite check. The **remote** host
only needs an ordinary `tmux` in `PATH`.

`LOCAL_TEST_ROOT` is a parent directory. Each run creates a fresh
`run-<pid>-<suffix>` directory below it and removes only that directory.
Whitespace in `LOCAL_TEST_ROOT` is rejected because paths are used in shell
commands and process listings.

## Prerequisites

Local (laptop):

- POSIX `sh`, `ssh`, `ps`, and standard utilities (`awk`, `sed`, `grep`, `tar`).
- A built `tmux` containing `new-remote-session` (or `TMUX_BIN` pointing at
  one).
- Working SSH authentication to the target, already verified with
  `ssh -o BatchMode=yes user@remote-host true` (or with the same
  `SSH_IDENTITY` setting).

Remote:

- SSH access for the user in `REMOTE_HOST`.
- `tmux`, `sh`, `tar`, `sed`, and `grep` in `PATH`.
- Permission to create and kill that user's own tmux sessions.
- A remote tmux new enough to support the forwarded `new-session` flags used by
  the command (`-A`, `-c`, `-e`, `-n`, `-x`, `-y`).

No root is required on either side.

## What the tests do

1. **Prerequisites and versions** — checks the local binary has
   `new-remote-session (newr)`, verifies BatchMode SSH reachability, prints the
   remote tmux version, and checks remote `tmux`, `sh`, `tar`, `sed`, `grep`.
2. **CLI create/attach and round trip** — runs the command under an OUTER tmux
   server that provides a real pty, verifies a local client is attached and the
   remote tmux session exists, then sends a POSIX `sh` command through the SSH
   pane and checks its computed output in `capture-pane` (so tty echo alone
   cannot satisfy the test).
3. **Metadata, formats and `list-sessions`** — checks `@remote_host`,
   `@remote_session`, optional `@remote_port`, `#{session_remote*}`,
   `#{pane_remote*}` and the `[remote host:session]` indicator.
4. **Persistent ControlMaster** — locates the helper's control path from the
   pane start command, verifies it is a Unix socket under
   `$XDG_RUNTIME_DIR/tmux-remote-<uid>/`, runs `ssh -O check`, performs a
   multiplexed `true`, and checks the helper uses `ControlMaster=auto`,
   `ControlPersist=yes` and keepalives.
5. **Flag forwarding** — uses `-s`, `-c /tmp`, `-e`, `-n`, `-x 91`, `-y 27`
   and a remote `shell-command`; verifies the remote session name, pane cwd,
   session environment, window name, `default-size` and computed command
   output.
6. **Attach/reuse** — attaches a second client to the existing local session
   and verifies there is no duplicate session, the SSH pane pid is unchanged,
   the control path is unchanged, and the remote session still exists.
7. **Config/plugin inheritance** — creates a disposable local `HOME` with a
   unique `@remote_real_test_marker`, `~/.tmux/plugins/real-test/`, and
   `~/.config/tmux/`, then verifies the copied cache directory on the remote,
   the copied plugin file, and (when the remote tmux server started fresh) the
   loaded marker option, `TMUX_PLUGIN_MANAGER_PATH`, plugin file and
   `TERM=xterm-256color`.
8. **Local detach survival** — detaches the nested client, verifies the local
   session and SSH pane pid survive, the control master remains healthy, and a
   new client can reattach without respawning the pane.
9. **Remote detach and ControlMaster** — runs `tmux detach-client` on the remote
   (independent of the remote prefix binding), with `remain-on-exit` forced off
   for this check, verifies the local SSH pane exits and its single-pane local
   session is destroyed, verifies the persistent control master survives, then
   reattaches and verifies the same control path and socket inode are reused.
10. **Hung SSH reconnect** — locates the local persistent mux master and its
    interactive SSH client for the test control path, `SIGSTOP`s both, runs the
    attach command again, and verifies the attach-time liveness path respawns
    the pane with a new pid, the old connection ends, the new connection is
    healthy, and a round trip works. This is a best-effort real-world
    simulation; see limitations.
11. **Inside-tmux modal pane** — attaches a pty client to an inner session, runs
    `new-remote-session` from that session's shell, and verifies the client
    stays on the original session, exactly one full-window modal pane exists
    with `#{pane_remote}`, its size is plausible, I/O works through it, and a
    second invocation completes and reuses the same pane id and pid.

## Safety and cleanup

- The harness uses a disposable local `HOME` for `~/.tmux` and `~/.tmux.conf`.
  It symlinks the real `~/.ssh` when present; SSH still uses the user's normal
  authentication (keys, config, agent).
- Local tmux servers use unique `-L` names containing the process id, time and
  a random suffix, and `-f /dev/null`. They are killed on exit.
- Remote test session names are unique (`remote_ssh_*_<pid>-<time>-<random>`).
  Cleanup kills only those exact names with `kill-session -t name:`. The
  harness **never** runs `kill-server` on the remote host.
- Remote test sessions are removed even if the local side has already failed.
  Cleanup runs from an exit/signal trap.
- Feature control masters are closed with `ssh -O exit`; any SSH master stopped
  by the hung-connection test is sent `SIGCONT`. A final scoped process scan
  targets only control paths under this run's private runtime directory.
- The disposable local test root and its short XDG runtime directory are
  removed at exit, unless `KEEP_TEST_ROOT=1`.
- The remote cache copy under `~/.cache/tmux-remote/<hash>/` is intentionally
  left in place, as documented by the feature. It does not modify the remote
  user's own `~/.tmux.conf`, `~/.tmux`, or `~/.config/tmux`.
- When a remote tmux server is already running, new uniquely named sessions are
  added to it. Existing clients are not switched or detached; only the test
  sessions are later killed. If you want complete isolation, run the tests on a
  remote account/host with no active tmux server.
- The harness unsets `TMUX` and `TMUX_PANE` in its own environment, so it can be
  started from inside a user's tmux session without creating modal panes in
  that session.

## Known limitations

- **Config loading needs a fresh remote server.** If the remote user's default
  tmux server is already running, `tmux -f <copied config>` cannot take effect.
  The harness still verifies that the config, plugin and `.config/tmux` files
  were copied, and reports `SKIP` for loading the marker option,
  `TMUX_PLUGIN_MANAGER_PATH` and `TERM`. Run against a remote account with no
  active tmux server to exercise the full inheritance path.
- **Hung-connection simulation is best effort.** OpenSSH may run the persistent
  mux master as a background `ssh: /path [mux]` process; the harness stops both
  that process and the pane's SSH client when it can identify them. On systems
  where the process listing hides the control path, the test is skipped. On
  some SSH versions the liveness check may fail quickly instead of hanging; the
  reconnect path is still exercised, but the five-second timeout itself may not
  be. The test allows up to 15 seconds for respawn.
- **Authentication must be pre-established.** Setup probes use
  `BatchMode=yes`. The real session may prompt for a password, but the test
  ptys will simply time out because no human is expected at the keyboard.
- **Remote tmux version and configuration differences.** The remote side must
  support the forwarded `new-session` flags. Very old tmux versions, unusual
  remote `default-shell` settings, or a user configuration that destroys
  detached sessions can make individual checks fail. Detaching the remote
  client is done with `detach-client`, so a custom remote prefix binding does
  not affect that test.
- **Plugin sync is best effort.** The helper copies `~/.tmux`, `~/.tmux.conf`
  and `~/.config/tmux` with `tar` and rewrites path references. The test checks
  the marker and copied files, not every plugin-manager behavior.
- **Local path restrictions.** `LOCAL_TEST_ROOT` must not contain whitespace.
  `REMOTE_HOST` must not contain whitespace or `|`. The private XDG runtime
  directory is placed under `/tmp` to keep Unix control-socket paths short.
- **Remote cache is not deleted.** This is deliberate: the harness avoids
  destructive recursive operations outside the exact sessions it created.

## Exit status and output

The script prints `PASS`, `FAIL` and `SKIP` lines and a final summary. Exit
status is `0` when no test failed, `1` for test or prerequisite failures, and
`2` for usage errors. `--check` only verifies the local binary and the
`new-remote-session` command; it never contacts `REMOTE_HOST`. A real run also
runs `sh -n` on the generated `cli`, `attach` and `inside` helper scripts before
starting any servers.

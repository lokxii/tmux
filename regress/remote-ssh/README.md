# Real SSH tests for the control-mode mirror

`regress/remote-ssh/run-mirror.sh` tests `new-remote-session` against a real
SSH host. The laptop is the local tmux host; `REMOTE_HOST` is the SSH target.

## Quick start

```sh
cd /path/to/tmux
REMOTE_HOST=user@remote-host sh regress/remote-ssh/run-mirror.sh
```

Authentication and host-key trust must already work:

```sh
ssh user@remote-host true
```

Optional environment:

```sh
REMOTE_HOST=user@remote-host REMOTE_PORT=2222 \
    sh regress/remote-ssh/run-mirror.sh

REMOTE_HOST=user@remote-host SSH_IDENTITY=$HOME/.ssh/id_ed25519 \
    sh regress/remote-ssh/run-mirror.sh

REMOTE_HOST=user@remote-host TMUX_BIN=/path/to/tmux \
    sh regress/remote-ssh/run-mirror.sh

sh regress/remote-ssh/run-mirror.sh --check
```

`TMUX_BIN` defaults to `tmux` in `PATH`. It must contain
`new-remote-session`.

## What is tested

- remote SSH reachability;
- mirror session creation and `@remote_host` / `@remote_session` metadata;
- `list-sessions` remote indicator;
- remote window and pane mirroring;
- remote output rendered locally and local input delivered remotely;
- remote `new-window`, `split-window`, `rename-window` routed through the
  control channel and mirrored back;
- `new-session` remaining local;
- copied config marker loading on the private remote tmux server;
- copied plugin file in `~/.cache/tmux-remote/<token>/`;
- cleanup of the local server and the private remote tmux server.

## Safety and cleanup

- Uses a disposable local `HOME` for the test config and plugin.
- Uses a unique local `-L` server and a unique local `TMUX_TMPDIR`.
- The remote private tmux server is identified by `@remote_token` on the local
  mirror session and killed with `tmux -L <token> kill-server` on cleanup.
- The remote user's default tmux server and config are not touched.
- The copied remote cache remains on the remote host, as documented by the
  feature.

## Known limitations

- The mirror currently does not seed initial screen contents/scrollback; only
  output after control attach is replayed.
- Mouse, images, clipboard/passthrough and flow control are not yet mirrored.
- Interactive remote modes are not forwarded.
- The older `run.sh` is retained for historical reference only; it tests the
  removed pty-proxy implementation.

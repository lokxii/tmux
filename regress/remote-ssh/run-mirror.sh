#!/bin/sh

# Real SSH integration test for the control-mode mirror.
#
# Run from the laptop:
#   REMOTE_HOST=user@host sh regress/remote-ssh/run-mirror.sh
#
# Optional: REMOTE_PORT, SSH_IDENTITY, TMUX_BIN.

set -u

REMOTE_HOST=${REMOTE_HOST:-}
REMOTE_PORT=${REMOTE_PORT:-}
SSH_IDENTITY=${SSH_IDENTITY:-}
TMUX_BIN=${TMUX_BIN:-}
PASS=0
FAIL=0
ROOT=
SOCK=
TOKEN=
SESSION=

usage()
{
	echo "usage: REMOTE_HOST=user@host [REMOTE_PORT=n] [SSH_IDENTITY=file]" >&2
	echo "       [TMUX_BIN=/path/to/tmux] sh $0 [--check]" >&2
	exit 2
}

fail()
{
	FAIL=$((FAIL + 1))
	echo "FAIL: $*"
}

ok()
{
	PASS=$((PASS + 1))
	echo "PASS: $*"
}

ssh_run()
{
	if [ -n "$REMOTE_PORT" ] && [ -n "$SSH_IDENTITY" ]; then
		ssh -o BatchMode=yes -o ConnectTimeout=10 -p "$REMOTE_PORT" \
		    -i "$SSH_IDENTITY" "$REMOTE_HOST" "$@" </dev/null
	elif [ -n "$REMOTE_PORT" ]; then
		ssh -o BatchMode=yes -o ConnectTimeout=10 -p "$REMOTE_PORT" \
		    "$REMOTE_HOST" "$@" </dev/null
	elif [ -n "$SSH_IDENTITY" ]; then
		ssh -o BatchMode=yes -o ConnectTimeout=10 -i "$SSH_IDENTITY" \
		    "$REMOTE_HOST" "$@" </dev/null
	else
		ssh -o BatchMode=yes -o ConnectTimeout=10 "$REMOTE_HOST" "$@" \
		    </dev/null
	fi
}

cleanup()
{
	if [ -n "$TMUX_BIN" ] && [ -n "$SOCK" ]; then
		"$TMUX_BIN" -L "$SOCK" kill-server >/dev/null 2>&1 || :
	fi
	if [ -n "$TOKEN" ]; then
		ssh_run "tmux -L '$TOKEN' kill-server" >/dev/null 2>&1 || :
	fi
	[ -n "$ROOT" ] && rm -rf "$ROOT"
}
trap cleanup 0 1 15

[ "${1:-}" = "--check" ] && CHECK=1 || CHECK=0
[ -z "$REMOTE_HOST" ] && [ "$CHECK" = 0 ] && usage
if [ -z "$TMUX_BIN" ]; then
	_here=$(CDPATH= cd "$(dirname "$0")" && pwd)
	if [ -x "$_here/../../tmux" ]; then
		TMUX_BIN=$_here/../../tmux
	else
		TMUX_BIN=$(command -v tmux 2>/dev/null)
	fi
fi
[ -z "$TMUX_BIN" ] && usage

_check_sock=mirror-check-$$
if ! "$TMUX_BIN" -L "$_check_sock" -f /dev/null list-commands 2>/dev/null |
    grep -q 'new-remote-session'; then
	echo "ERROR: $TMUX_BIN has no new-remote-session command" >&2
	"$TMUX_BIN" -L "$_check_sock" kill-server >/dev/null 2>&1 || :
	exit 1
fi
"$TMUX_BIN" -L "$_check_sock" kill-server >/dev/null 2>&1 || :
if [ "$CHECK" = 1 ]; then
	echo "local tmux: $TMUX_BIN"
	exit 0
fi

ssh_run true || {
	echo "ERROR: cannot ssh to $REMOTE_HOST" >&2
	exit 1
}

ROOT=$(mktemp -d "${TMPDIR:-/tmp}/tmux-mirror-ssh.XXXXXX") || exit 1
mkdir -p "$ROOT/home/.tmux/plugins/mirror-test" "$ROOT/home/.config/tmux"
cat >"$ROOT/home/.tmux.conf" <<EOF
set -g @mirror_marker mirror-$$
set -g status-left "[mirror-test] "
EOF
echo marker >"$ROOT/home/.tmux/plugins/mirror-test/plugin.tmux"
HOME=$ROOT/home
TMUX_TMPDIR=$ROOT
SOCK=mirror-ssh-$$
SESSION=mirror-ssh-session-$$
export HOME TMUX_TMPDIR

"$TMUX_BIN" -L "$SOCK" -f /dev/null new-session -d -s host 'exec sleep 300' ||
	exit 1

if [ -n "$REMOTE_PORT" ] && [ -n "$SSH_IDENTITY" ]; then
	"$TMUX_BIN" -L "$SOCK" -f /dev/null new-remote-session -d \
	    -H "$REMOTE_HOST" -p "$REMOTE_PORT" -i "$SSH_IDENTITY" \
	    -s "$SESSION"
elif [ -n "$REMOTE_PORT" ]; then
	"$TMUX_BIN" -L "$SOCK" -f /dev/null new-remote-session -d \
	    -H "$REMOTE_HOST" -p "$REMOTE_PORT" -s "$SESSION"
elif [ -n "$SSH_IDENTITY" ]; then
	"$TMUX_BIN" -L "$SOCK" -f /dev/null new-remote-session -d \
	    -H "$REMOTE_HOST" -i "$SSH_IDENTITY" -s "$SESSION"
else
	"$TMUX_BIN" -L "$SOCK" -f /dev/null new-remote-session -d \
	    -H "$REMOTE_HOST" -s "$SESSION"
fi || {
	fail "new-remote-session failed"
	exit 1
}
ok "new-remote-session started"

i=0
while ! "$TMUX_BIN" -L "$SOCK" has-session -t "$SESSION" 2>/dev/null; do
	i=$((i + 1))
	[ "$i" -gt 100 ] && {
		fail "mirror session did not appear"
		exit 1
	}
	sleep 0.1
done
ok "mirror session created"

TOKEN=$("$TMUX_BIN" -L "$SOCK" show-options -t "$SESSION" -v @remote_token 2>/dev/null)
[ -n "$TOKEN" ] && ok "remote token recorded" || fail "no remote token"

[ "$("$TMUX_BIN" -L "$SOCK" show-options -t "$SESSION" -v @remote_host)" = \
    "$REMOTE_HOST" ] && ok "remote host metadata" || fail "bad host metadata"

"$TMUX_BIN" -L "$SOCK" list-sessions |
    grep -F "[remote $REMOTE_HOST:$SESSION]" >/dev/null &&
    ok "list-sessions remote indicator" || fail "no remote indicator"

i=0
while [ "$("$TMUX_BIN" -L "$SOCK" list-windows -t "$SESSION" -F x |
    wc -l)" -lt 1 ]; do
	i=$((i + 1))
	[ "$i" -gt 100 ] && break
	sleep 0.1
done
ok "remote window mirrored"

"$TMUX_BIN" -L "$SOCK" send-keys -t "$SESSION:0.0" 'echo MIRROR_SSH_OK' Enter
i=0
while ! "$TMUX_BIN" -L "$SOCK" capture-pane -p -t "$SESSION:0.0" |
    grep -F MIRROR_SSH_OK >/dev/null; do
	i=$((i + 1))
	[ "$i" -gt 100 ] && break
	sleep 0.1
done
[ "$i" -le 100 ] && ok "input/output round trip" || fail "I/O round trip"

"$TMUX_BIN" -L "$SOCK" new-window -t "$SESSION"
i=0
while [ "$("$TMUX_BIN" -L "$SOCK" list-windows -t "$SESSION" -F x |
    wc -l)" -ne 2 ]; do
	i=$((i + 1))
	[ "$i" -gt 100 ] && break
	sleep 0.1
done
[ "$i" -le 100 ] && ok "remote new-window mirrored" ||
	fail "remote new-window not mirrored"

"$TMUX_BIN" -L "$SOCK" rename-window -t "$SESSION:1" renamed
i=0
while [ "$("$TMUX_BIN" -L "$SOCK" list-windows -t "$SESSION" \
    -F '#{window_name}' | tail -1)" != renamed ]; do
	i=$((i + 1))
	[ "$i" -gt 100 ] && break
	sleep 0.1
done
[ "$i" -le 100 ] && ok "remote rename mirrored" || fail "rename not mirrored"

"$TMUX_BIN" -L "$SOCK" split-window -t "$SESSION:0"
i=0
while [ "$("$TMUX_BIN" -L "$SOCK" list-panes -t "$SESSION:0" -F x |
    wc -l)" -ne 2 ]; do
	i=$((i + 1))
	[ "$i" -gt 100 ] && break
	sleep 0.1
done
[ "$i" -le 100 ] && ok "remote split mirrored" || fail "split not mirrored"

if [ -n "$TOKEN" ]; then
	marker=$(ssh_run "tmux -L '$TOKEN' show-options -g -v @mirror_marker" \
	    2>/dev/null)
	[ "$marker" = "mirror-$$" ] && ok "copied config loaded remotely" ||
	    fail "copied config marker not loaded"
	ssh_run "test -f ~/.cache/tmux-remote/'$TOKEN'/.tmux/plugins/mirror-test/plugin.tmux" &&
	    ok "plugin copied remotely" || fail "plugin not copied"
fi

"$TMUX_BIN" -L "$SOCK" new-session -d -s local-only 'exec sleep 300' ||
	fail "local new-session failed"
"$TMUX_BIN" -L "$SOCK" has-session -t local-only &&
	ok "new-session stayed local" || fail "new-session was forwarded"
"$TMUX_BIN" -L "$SOCK" kill-session -t local-only >/dev/null 2>&1 || :

echo
echo "Summary: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]

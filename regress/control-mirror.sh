#!/bin/sh
#
# Very small end-to-end test for the experimental control-mode mirror.
#
# It uses two local tmux servers: one plays the remote side and is attached in
# control mode by the mirror; the other is the local server which owns the
# mirrored session.  No ssh is involved.
#
# Usage: regress/control-mirror.sh [path-to-tmux]

set -eu

if [ $# -gt 0 ]; then
	TMUX_BIN=$1
else
	TMUX_BIN=$(cd "$(dirname "$0")/.." && pwd)/tmux
fi
case "$TMUX_BIN" in
/*) ;;
*) TMUX_BIN=$(cd "$(dirname "$TMUX_BIN")" && pwd)/$(basename "$TMUX_BIN") ;;
esac

L="mirror-local-$$"
R="mirror-remote-$$"
FAILS=0

cleanup() {
	"$TMUX_BIN" -L "$L" kill-server >/dev/null 2>&1 || :
	"$TMUX_BIN" -L "$R" kill-server >/dev/null 2>&1 || :
}
trap cleanup EXIT INT TERM

fail() {
	echo "FAIL: $*"
	FAILS=$((FAILS + 1))
}

pass() {
	echo "PASS: $*"
}

# Wait for a command output to match a pattern, up to about five seconds.
wait_for() {
	what=$1
	shift
	i=0
	while [ $i -lt 50 ]; do
		if "$@" 2>/dev/null; then
			pass "$what"
			return 0
		fi
		i=$((i + 1))
		sleep 0.1
	done
	fail "$what"
	return 1
}

local_has() {
	"$TMUX_BIN" -L "$L" list-windows -t mirror -F '#{window_name}' |
	    grep -qx "$1"
}

local_count() {
	n=$("$TMUX_BIN" -L "$L" list-windows -t mirror -F 'x' 2>/dev/null | wc -l)
	[ "$n" -eq "$1" ]
}

local_active() {
	"$TMUX_BIN" -L "$L" list-windows -t mirror -F \
	    '#{window_index}:#{window_active}' | grep -qx "$1:1"
}

local_layout_has() {
	"$TMUX_BIN" -L "$L" list-windows -t mirror -F '#{window_layout}' |
	    grep -q "$1"
}

local_capture_has() {
	"$TMUX_BIN" -L "$L" capture-pane -p -t mirror:0.0 |
	    grep -q "$1"
}

remote_capture_has() {
	"$TMUX_BIN" -L "$R" capture-pane -p -t mirror:0.0 |
	    grep -q "$1"
}

echo "tmux binary: $TMUX_BIN"

# Remote side: a normal detached session with a shell.
"$TMUX_BIN" -L "$R" -f /dev/null new-session -d -s mirror
"$TMUX_BIN" -L "$R" list-sessions >/dev/null

# Local side: a server with its own session so the server stays alive.
"$TMUX_BIN" -L "$L" -f /dev/null new-session -d -s host
"$TMUX_BIN" -L "$L" list-sessions >/dev/null

# Start the mirror.  The remote command is run by the local server through
# job_run(), exactly where ssh would be in a real integration.
"$TMUX_BIN" -L "$L" mirror -s mirror \
    "env -u TMUX $TMUX_BIN -L $R -f /dev/null -C attach -t mirror"

wait_for "mirror session created" \
    "$TMUX_BIN" -L "$L" has-session -t mirror
wait_for "one mirrored window" local_count 1
wait_for "window name mirrored" local_has bash

# Remote output -> local pane.
"$TMUX_BIN" -L "$R" send-keys -t mirror:0.0 -l 'echo MIRROR_REMOTE_OUT'
"$TMUX_BIN" -L "$R" send-keys -t mirror:0.0 Enter
wait_for "remote output appears locally" local_capture_has MIRROR_REMOTE_OUT

# Local input -> remote pane (via send-keys -lH).
"$TMUX_BIN" -L "$L" send-keys -t mirror:0.0 -l 'echo MIRROR_LOCAL_KEY'
"$TMUX_BIN" -L "$L" send-keys -t mirror:0.0 Enter
wait_for "local input reaches remote" remote_capture_has MIRROR_LOCAL_KEY

# Layout change: split horizontally on the remote.
"$TMUX_BIN" -L "$R" split-window -h -t mirror
wait_for "split layout mirrored" local_layout_has '"t":"h"'
n=$("$TMUX_BIN" -L "$L" list-panes -s -t mirror -F 'x' | wc -l)
if [ "$n" -eq 2 ]; then
	pass "two local panes after split"
else
	fail "expected two local panes, got $n"
fi

# Window add, rename, selection and close.
"$TMUX_BIN" -L "$R" new-window -d -t mirror -n second
wait_for "window added" local_count 2
"$TMUX_BIN" -L "$R" rename-window -t mirror:1 renamed
wait_for "window renamed" local_has renamed
"$TMUX_BIN" -L "$R" select-window -t mirror:1
wait_for "window selection mirrored" local_active 1
"$TMUX_BIN" -L "$R" kill-window -t mirror:1
wait_for "window close mirrored" local_count 1

# %exit / EOF: kill the remote server, local mirror session must go away but
# the local server and its own session must survive.
"$TMUX_BIN" -L "$R" kill-server
wait_for "mirror session removed after %exit" \
    sh -c "! $TMUX_BIN -L $L has-session -t mirror"
if "$TMUX_BIN" -L "$L" has-session -t host 2>/dev/null; then
	pass "local server survived remote exit"
else
	fail "local server disappeared"
fi

if [ "$FAILS" -ne 0 ]; then
	echo "$FAILS failure(s)"
	exit 1
fi
echo "all tests passed"

#!/bin/sh

# Exercise new-remote-session without a real remote host: a fake ssh records
# the command line and stays alive until killed.

. ./regress-common.inc

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
DIR=$(test_tmpdir) || exit 1
TMUX_TMPDIR=$DIR
export TMUX_TMPDIR

mkdir -p "$DIR/bin" "$DIR/home"
cat >"$DIR/bin/ssh" <<'EOF'
#!/bin/sh
printf '%s\n' "$*" >>"$FAKE_SSH_LOG"
case "$*" in
*"-O exit"*)
	exit 0
	;;
*"mkdir -p "*"tar -C "*"-xf -"*)
	cat >/dev/null
	exit 0
	;;
esac
exec sleep 100
EOF
chmod +x "$DIR/bin/ssh"

HOME=$DIR/home
PATH=$DIR/bin:/bin:/usr/bin
FAKE_SSH_LOG=$DIR/ssh.log
TMUX="$TEST_TMUX -Ltest$$ -f/dev/null"
export HOME PATH FAKE_SSH_LOG

cleanup()
{
	stop_server $TMUX || exit 1
	rm -rf "$DIR"
}
trap cleanup 0 1 15

: >"$FAKE_SSH_LOG"
$TMUX new-remote-session -d -H example.test -s remote-test || exit 1

# The session is marked as remote and starts a remote tmux new-session -A.
$TMUX list-sessions | grep -F '[remote example.test:remote-test]' >/dev/null ||
	exit 1
[ "$($TMUX list-sessions -F '#{session_remote}|#{session_remote_session}')" = \
    'example.test|remote-test' ] || exit 1
[ "$($TMUX show-options -t remote-test -v @remote_host)" = example.test ] ||
	exit 1
[ "$($TMUX show-options -t remote-test -v @remote_session)" = remote-test ] ||
	exit 1
[ "$($TMUX show-options -p -t remote-test:0.0 -v remain-on-exit)" = on ] ||
	exit 1
grep -F "new-session" "$FAKE_SSH_LOG" | grep -F "'-A'" |
	grep -F "'-s' 'remote-test'" >/dev/null || exit 1

# Exiting ssh leaves the local session and pane alive.
pid=$($TMUX list-panes -t remote-test -F '#{pane_pid}') || exit 1
kill "$pid" || exit 1
sleep 1
[ "$($TMUX list-panes -t remote-test -F '#{pane_dead}')" = 1 ] || exit 1
[ "$($TMUX list-sessions -F '#{session_name}')" = remote-test ] || exit 1

# Invoking the command again reattaches the session and respawns the pane
# without killing the persistent connection.
$TMUX new-remote-session -d -H example.test -s remote-test || exit 1
sleep 1
[ "$($TMUX list-panes -t remote-test -F '#{pane_dead}')" = 0 ] || exit 1
[ "$($TMUX list-sessions -F '#{session_name}')" = remote-test ] || exit 1
grep -F -- '-O exit' "$FAKE_SSH_LOG" >/dev/null && exit 1

# -R forces a fresh SSH connection.
: >"$FAKE_SSH_LOG"
$TMUX new-remote-session -d -R -H example.test -s remote-test || exit 1
sleep 1
grep -F -- '-O exit' "$FAKE_SSH_LOG" >/dev/null || exit 1

# A different host cannot reuse the same remote session name.
$TMUX new-remote-session -d -H other.test -s remote-test >/dev/null 2>&1 &&
	exit 1
exit 0

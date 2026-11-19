#!/bin/sh

# Exercise new-remote-session without a real remote host: a fake ssh records
# the command line and stays alive until killed.

. ./regress-common.inc

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
DIR=$(test_tmpdir) || exit 1
TMUX_TMPDIR=$DIR
export TMUX_TMPDIR

mkdir -p "$DIR/bin" "$DIR/home"
cat >"$DIR/bin/ssh" <<EOF
#!/bin/sh
printf '%s\n' "\$*" >>"$DIR/ssh.log"
case "\$*" in
*"-O check"*)
	if [ -f "$DIR/check-hang" ]; then
		exec sleep 10
	fi
	exit 0
	;;
*"-O exit"*)
	exit 0
	;;
*" true")
	exit 0
	;;
*"mkdir -p "*"tar -C "*"-xf -"*)
	cat >/dev/null
	exit 0
	;;
esac
printf 'REMOTE-OUT\n'
while IFS= read -r line; do
	printf 'REMOTE-IN:%s\n' "\$line"
done
EOF
chmod +x "$DIR/bin/ssh"

HOME=$DIR/home
PATH=$DIR/bin:/bin:/usr/bin
FAKE_SSH_LOG=$DIR/ssh.log
FAKE_CHECK_HANG=$DIR/check-hang
TMUX="$TEST_TMUX -Ltest$$ -f/dev/null"
OUTER="$TEST_TMUX -Louter$$ -f/dev/null"
export HOME PATH FAKE_SSH_LOG FAKE_CHECK_HANG

cleanup()
{
	stop_server $OUTER || :
	stop_server $TMUX || exit 1
	rm -rf "$DIR"
}
trap cleanup 0 1 15

remote_pane()
{
	$TMUX list-panes -t inside-main:0 \
	    -F '#{pane_id}:#{pane_remote}:#{pane_pid}' |
	    grep ':inside.test:' | head -1
}

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

# Use a second tmux server as the attached client's terminal, so the command
# runs from a pane inside a real (pty-backed) tmux session.
$TMUX new-session -d -s inside-main || exit 1
$OUTER new-session -d -s out -x 80 -y 24 \
    "$TMUX attach -t inside-main" || exit 1
i=0
while [ -z "$($TMUX list-clients -F '#{client_session}')" ]; do
	i=$((i + 1))
	[ "$i" -gt 50 ] && exit 1
	sleep 0.1
done

# Inside tmux the remote session must appear as a full-size modal pane in the
# current window, and the client must stay on its original session.
$TMUX send-keys -t inside-main:0.0 \
    "$TEST_TMUX -Ltest$$ new-remote-session -H inside.test -s popup" Enter ||
	exit 1
i=0
while [ -z "$(remote_pane)" ]; do
	i=$((i + 1))
	[ "$i" -gt 50 ] && exit 1
	sleep 0.1
done
[ "$($TMUX list-clients -F '#{client_session}')" = inside-main ] || exit 1
[ "$($TMUX show-options -w -t inside-main:0 -v @remote_host)" = inside.test ] ||
	exit 1
[ "$($TMUX show-options -p -t "$(remote_pane | cut -d: -f1)" \
    -v remain-on-exit)" = on ] || exit 1

# Output from the SSH pane is rendered by the modal pane, and input sent to
# the modal pane reaches the SSH child.
line=$(remote_pane)
modal_id=${line%%:*}
$TMUX capture-pane -p -t "$modal_id" | grep -F 'REMOTE-OUT' >/dev/null ||
	exit 1
$TMUX send-keys -t "$modal_id" 'hello' Enter || exit 1
i=0
while ! $TMUX capture-pane -p -t "$modal_id" |
    grep -F 'REMOTE-IN:hello' >/dev/null; do
	i=$((i + 1))
	[ "$i" -gt 30 ] && exit 1
	sleep 0.1
done

# A healthy existing modal pane is reused without reconnecting.
old_id=${line%%:*}
old_pid=${line##*:}
: >"$FAKE_SSH_LOG"
$TMUX send-keys -t inside-main:0.0 \
    "$TEST_TMUX -Ltest$$ new-remote-session -H inside.test -s popup" Enter ||
	exit 1
sleep 1
line=$(remote_pane)
[ "${line%%:*}" = "$old_id" ] || exit 1
[ "${line##*:}" = "$old_pid" ] || exit 1
grep -F -- '-O exit' "$FAKE_SSH_LOG" >/dev/null && exit 1

# A hung liveness check must kill and re-establish the SSH connection.
touch "$FAKE_CHECK_HANG"
: >"$FAKE_SSH_LOG"
$TMUX send-keys -t inside-main:0.0 \
    "$TEST_TMUX -Ltest$$ new-remote-session -H inside.test -s popup" Enter ||
	exit 1
i=0
while :; do
	i=$((i + 1))
	[ "$i" -gt 100 ] && exit 1
	line=$(remote_pane)
	if [ -n "$line" ] && [ "${line##*:}" != "$old_pid" ]; then
		break
	fi
	sleep 0.1
done
i=0
while ! grep -F -- '-O exit' "$FAKE_SSH_LOG" >/dev/null; do
	i=$((i + 1))
	[ "$i" -gt 30 ] && exit 1
	sleep 0.1
done
exit 0

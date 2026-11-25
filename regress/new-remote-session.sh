#!/bin/sh

# Exercise new-remote-session using a fake ssh which runs the "remote"
# control-mode command locally. This validates the local mirror without a
# network.

. ./regress-common.inc

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
DIR=$(test_tmpdir) || exit 1
TMUX_TMPDIR=$DIR
export TMUX_TMPDIR

mkdir -p "$DIR/bin" "$DIR/home"
cat >"$DIR/bin/ssh" <<EOF
#!/bin/sh
printf '%s\n' "\$*" >>"$DIR/ssh.log"
last=
for a in "\$@"; do last=\$a; done
case "\$last" in
mkdir\ -p\ *)
	cat >/dev/null
	exit 0
	;;
esac
exec sh -c "\$last"
EOF
chmod +x "$DIR/bin/ssh"

TEST_TMUX_DIR=$(dirname "$(readlink -f "$TEST_TMUX")")
HOME=$DIR/home
PATH=$DIR/bin:$TEST_TMUX_DIR:/bin:/usr/bin
TMUX="$TEST_TMUX -Ltest$$ -f/dev/null"
export HOME PATH

cleanup()
{
	stop_server $TMUX || exit 1
	rm -rf "$DIR"
}
trap cleanup 0 1 15

wait_session()
{
	i=0
	while ! $TMUX has-session -t "$1" 2>/dev/null; do
		i=$((i + 1))
		[ "$i" -gt 60 ] && return 1
		sleep 0.1
	done
}

wait_windows()
{
	i=0
	while [ "$($TMUX list-windows -t "$1" -F '#{window_index}' |
	    wc -l)" -ne "$2" ]; do
		i=$((i + 1))
		[ "$i" -gt 60 ] && return 1
		sleep 0.1
	done
}

: >"$DIR/ssh.log"
$TMUX new-remote-session -d -H example.test -s remote-test || exit 1
wait_session example_test~remote-test || exit 1

# The local mirror name is prefixed with the sanitized remote host.
$TMUX list-sessions -F '#{session_name}' |
    grep -Fx 'example_test~remote-test' >/dev/null || exit 1

# Session metadata, formats and the remote indicator.
[ "$($TMUX show-options -t example_test~remote-test -v @remote_host)" = \
    example.test ] || exit 1
[ "$($TMUX show-options -t example_test~remote-test -v @remote_session)" = \
    remote-test ] || exit 1
$TMUX list-sessions | grep -F '[remote example.test:remote-test]' >/dev/null ||
	exit 1
[ "$($TMUX list-sessions -F '#{session_remote}|#{session_remote_session}')" = \
    'example.test|remote-test' ] || exit 1

# One mirrored window and pane.
wait_windows example_test~remote-test 1 || exit 1
[ "$($TMUX list-panes -t example_test~remote-test -F '#{pane_remote}')" = \
    example.test ] || exit 1

# Output from the remote pane appears locally, and local input reaches it.
$TMUX send-keys -t example_test~remote-test:0.0 'echo MIRROR_OK_123' Enter ||
	exit 1
i=0
while ! $TMUX capture-pane -p -t example_test~remote-test:0.0 |
    grep -F 'MIRROR_OK_123' >/dev/null; do
	i=$((i + 1))
	[ "$i" -gt 60 ] && exit 1
	sleep 0.1
done

# new-window is routed to the remote control stream and mirrored back.
$TMUX new-window -t example_test~remote-test || exit 1
wait_windows example_test~remote-test 2 || exit 1
name=$($TMUX list-windows -t example_test~remote-test -F '#{window_name}' |
    tail -1)
[ -n "$name" ] || exit 1

# split-window is also remote; the mirror gains a second pane.
$TMUX split-window -t example_test~remote-test:0 || exit 1
i=0
while [ "$($TMUX list-panes -t example_test~remote-test:0 -F '#{pane_id}' |
    wc -l)" -ne 2 ]; do
	i=$((i + 1))
	[ "$i" -gt 60 ] && exit 1
	sleep 0.1
done

# New-session is local, not forwarded.
$TMUX new-session -d -s local-only 'exec sleep 100' || exit 1
$TMUX has-session -t local-only || exit 1
$TMUX kill-session -t local-only || exit 1

# -i is passed to the SSH helper.
: >"$DIR/ssh.log"
$TMUX new-remote-session -d -H example.test -s key-test \
    -i /tmp/fake-identity || exit 1
wait_session example_test~key-test || exit 1
grep -F -- '-i /tmp/fake-identity' "$DIR/ssh.log" >/dev/null || exit 1
$TMUX kill-session -t example_test~key-test || exit 1

# The same remote session name on a different host is a separate mirror: the
# local name is prefixed with the host, so both can exist at once.
$TMUX new-remote-session -d -H other.test -s remote-test || exit 1
wait_session other_test~remote-test || exit 1
$TMUX has-session -t example_test~remote-test || exit 1
$TMUX has-session -t other_test~remote-test || exit 1

# Re-running it reuses that mirror rather than creating a duplicate.
$TMUX new-remote-session -d -H other.test -s remote-test || exit 1
[ "$($TMUX list-sessions -F '#{session_name}' |
    grep -Fxc 'other_test~remote-test')" -eq 1 ] || exit 1
$TMUX kill-session -t other_test~remote-test || exit 1
$TMUX kill-session -t example_test~remote-test || exit 1

# The default name is derived from the host with dots removed: a name with
# dots cannot be attached by name (tmux treats '.' as a pane separator).
$TMUX new-remote-session -d -H example.test || exit 1
wait_session remote-example_test || exit 1
[ "$($TMUX list-sessions -F '#{@remote_session}' |
    grep -F remote-example_test)" = remote-example_test ] || exit 1
$TMUX kill-session -t remote-example_test || exit 1

# Explicit dotted and colon names are normalized for the same reason; the
# local mirror is also prefixed with the sanitized host.
$TMUX new-remote-session -d -H example.test -s dotted.name || exit 1
wait_session example_test~dotted_name || exit 1
$TMUX has-session -t example_test~dotted_name || exit 1
$TMUX kill-session -t example_test~dotted_name || exit 1
$TMUX new-remote-session -d -H example.test -s 'a:b.c' || exit 1
wait_session example_test~a_b_c || exit 1
$TMUX kill-session -t example_test~a_b_c || exit 1
exit 0

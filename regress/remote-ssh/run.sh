#!/bin/sh

# Real SSH integration tests for tmux new-remote-session (newr).
#
# The machine running this script is the local tmux host.  REMOTE_HOST names a
# real SSH target which runs the remote tmux server.  The test creates only
# disposable local tmux servers, a disposable local HOME and uniquely named
# remote tmux sessions.  It never runs kill-server on the remote.
#
# Usage:
#   REMOTE_HOST=user@host sh regress/remote-ssh/run.sh
#   REMOTE_HOST=user@host REMOTE_PORT=2222 \
#       TMUX_BIN=/path/to/tmux sh regress/remote-ssh/run.sh
#   sh regress/remote-ssh/run.sh --check
#
# The script is POSIX sh and does not require root, Python or expect.

set -u

SCRIPT_DIR=$(CDPATH= cd "$(dirname "$0")" && pwd) || exit 1
REPO_ROOT=$(CDPATH= cd "$SCRIPT_DIR/../.." && pwd) || exit 1

PASSED=0
FAILED=0
SKIPPED=0

REMOTE_HOST=${REMOTE_HOST:-}
REMOTE_PORT=${REMOTE_PORT:-}
SSH_IDENTITY=${SSH_IDENTITY:-}
TMUX_BIN=${TMUX_BIN:-}
LOCAL_TEST_ROOT=${LOCAL_TEST_ROOT:-}
KEEP_TEST_ROOT=${KEEP_TEST_ROOT:-0}

ROOT=
RUNTIME=
HOME_DIR=
PROBE_CTL=
CHECK_SOCK=
REAL_HOME=${HOME:-}
UID_NUM=$(id -u)
SOCK=
OUTERSOCK=
SUFFIX=
MARKER=
MAIN=
FLAGS=
MODAL=
MODAL_SESSION=
REMOTE_SERVER_PREEXISTING=0
OUTER_SEQ=0
LAST_OUTER=
STOPPED_PIDS=
MAIN_READY=0
MAIN_LIVE=0
MAIN_CTL=

usage()
{
	cat <<EOF
Usage: REMOTE_HOST=user@host sh $0 [--help|--check]

Run real SSH integration tests for tmux new-remote-session.  This machine
acts as the local tmux host; REMOTE_HOST is a reachable SSH target.

Environment:
  REMOTE_HOST       required for a real run, for example user@server
  REMOTE_PORT       optional SSH port
  SSH_IDENTITY      optional SSH private key passed to ssh and -i
  TMUX_BIN          local tmux binary (default: $REPO_ROOT/tmux if
                    executable, otherwise "tmux" from PATH)
  LOCAL_TEST_ROOT   optional parent directory for the disposable test root
  KEEP_TEST_ROOT    set to 1 to keep local test files after a run

Options:
  --check           only check the local tmux binary and command; does not
                    contact the remote host
  -h, --help        show this help

The test does not modify the remote user's configuration.  It creates
uniquely named remote tmux sessions and removes only those sessions.
EOF
}

inventory()
{
	cat <<EOF
Test inventory:
  1. prerequisites: local tmux command, remote SSH, tmux, sh, tar, sed and grep
  2. CLI create/attach: local session, client, remote session, I/O round trip
  3. session metadata, formats and list-sessions remote indicator
  4. persistent ControlMaster path, check and keepalive options
  5. flag forwarding: -s -c -e -n -x -y and a remote shell-command
  6. attach/reuse: no duplicate session and the same pane pid
  7. config/plugin inheritance via the remote cache directory
  8. local detach survival: the SSH pane stays alive and can be reattached
  9. remote detach: the ControlMaster survives and is reused
 10. hung SSH reconnect: SIGSTOP the local master, then attach again
 11. inside-tmux full-window modal pane creation, I/O and reuse
EOF
}

pass()
{
	PASSED=$((PASSED + 1))
	printf 'PASS: %s\n' "$*"
}

fail()
{
	FAILED=$((FAILED + 1))
	printf 'FAIL: %s\n' "$*"
}

skip()
{
	SKIPPED=$((SKIPPED + 1))
	printf 'SKIP: %s\n' "$*"
}

note()
{
	printf 'INFO: %s\n' "$*"
}

section()
{
	printf '\n== %s ==\n' "$*"
}

summary()
{
	printf '\nSummary: %d passed, %d failed, %d skipped\n' \
	    "$PASSED" "$FAILED" "$SKIPPED"
}

fatal()
{
	fail "$*"
	summary
	exit 1
}

assert_eq()
{
	if [ "$2" = "$3" ]; then
		pass "$1"
	else
		fail "$1 (expected '$2', got '$3')"
	fi
}

assert_ne()
{
	if [ "$2" != "$3" ]; then
		pass "$1"
	else
		fail "$1 (both values are '$2')"
	fi
}

assert_contains()
{
	case $3 in
	*"$2"*) pass "$1" ;;
	*) fail "$1 (output does not contain '$2')" ;;
	esac
}

assert_ok()
{
	_ao_desc=$1
	shift
	if "$@"; then
		pass "$_ao_desc"
	else
		fail "$_ao_desc"
	fi
}

assert_number_between()
{
	# desc value minimum maximum
	case $2 in
	''|*[!0-9]*)
		fail "$1 (not a number: '$2')"
		return
		;;
	esac
	if [ "$2" -ge "$3" ] && [ "$2" -le "$4" ]; then
		pass "$1 ($2)"
	else
		fail "$1 ($2 is not between $3 and $4)"
	fi
}

random_suffix()
{
	awk 'BEGIN { srand(); printf "%06d", int(rand() * 1000000) }'
}

# Quote a single string for a POSIX shell.  The result includes the quotes.
quote_sh()
{
	printf "'%s'" "$(printf '%s' "$1" | sed "s/'/'\\\\''/g")"
}

timeout_cmd()
{
	_tc_secs=$1
	shift
	"$@" &
	_tc_pid=$!
	_tc_n=0
	_tc_max=$((_tc_secs * 10))
	while kill -0 "$_tc_pid" 2>/dev/null; do
		if [ "$_tc_n" -ge "$_tc_max" ]; then
			kill -s TERM "$_tc_pid" 2>/dev/null || :
			sleep 0.2
			kill -s KILL "$_tc_pid" 2>/dev/null || :
			wait "$_tc_pid" 2>/dev/null || :
			return 124
		fi
		_tc_n=$((_tc_n + 1))
		sleep 0.1
	done
	wait "$_tc_pid"
}

wait_for()
{
	_wf_secs=$1
	_wf_desc=$2
	shift 2
	_wf_n=0
	_wf_max=$((_wf_secs * 10))
	while [ "$_wf_n" -le "$_wf_max" ]; do
		if "$@"; then
			return 0
		fi
		_wf_n=$((_wf_n + 1))
		sleep 0.1
	done
	printf 'INFO: timed out waiting for %s\n' "$_wf_desc" >&2
	return 1
}

stop_server()
{
	if [ "$#" -eq 0 ]; then
		return 0
	fi
	_ss_pid=$("$@" display-message -p '#{pid}' 2>/dev/null) || _ss_pid=
	"$@" kill-server >/dev/null 2>&1 || :
	if [ -n "$_ss_pid" ]; then
		_ss_n=0
		while kill -0 "$_ss_pid" 2>/dev/null && [ "$_ss_n" -lt 200 ]; do
			_ss_n=$((_ss_n + 1))
			sleep 0.05
		done
		if kill -0 "$_ss_pid" 2>/dev/null; then
			return 1
		fi
	fi
	return 0
}

tmux_main()
{
	"$TMUX_BIN" -L "$SOCK" -f /dev/null "$@"
}

tmux_outer()
{
	"$TMUX_BIN" -L "$OUTERSOCK" -f /dev/null "$@"
}

new_remote_main()
{
	if [ -n "$REMOTE_PORT" ] && [ -n "$SSH_IDENTITY" ]; then
		tmux_main new-remote-session -H "$REMOTE_HOST" \
		    -p "$REMOTE_PORT" -i "$SSH_IDENTITY" "$@"
	elif [ -n "$REMOTE_PORT" ]; then
		tmux_main new-remote-session -H "$REMOTE_HOST" \
		    -p "$REMOTE_PORT" "$@"
	elif [ -n "$SSH_IDENTITY" ]; then
		tmux_main new-remote-session -H "$REMOTE_HOST" \
		    -i "$SSH_IDENTITY" "$@"
	else
		tmux_main new-remote-session -H "$REMOTE_HOST" "$@"
	fi
}

# tmux can leave an empty socket file behind after kill-server.  Remove only
# the uniquely named sockets created by this run.
remove_local_sockets()
{
	for _rls_name in "$SOCK" "$OUTERSOCK" "$CHECK_SOCK"; do
		[ -n "$_rls_name" ] || continue
		for _rls_dir in "${TMUX_TMPDIR:-/tmp}/tmux-$UID_NUM" \
		    "/tmp/tmux-$UID_NUM"; do
			rm -f "$_rls_dir/$_rls_name" 2>/dev/null || :
		done
	done
}

local_sanity()
{
	if [ ! -x "$TMUX_BIN" ]; then
		printf 'ERROR: local tmux binary is not executable: %s\n' \
		    "$TMUX_BIN" >&2
		return 1
	fi
	if ! command -v ssh >/dev/null 2>&1; then
		printf 'ERROR: ssh is not available in PATH\n' >&2
		return 1
	fi
	_ls_ver=$("$TMUX_BIN" -V 2>&1) || _ls_ver="unknown"
	printf 'local tmux: %s (%s)\n' "$TMUX_BIN" "$_ls_ver"

	_ls_probe="tssh-check-$$-$(random_suffix)"
	CHECK_SOCK=$_ls_probe
	_ls_out=$("$TMUX_BIN" -L "$_ls_probe" -f /dev/null \
	    list-commands 2>&1) || {
		printf 'ERROR: cannot run local tmux: %s\n' "$_ls_out" >&2
		"$TMUX_BIN" -L "$_ls_probe" kill-server >/dev/null 2>&1 || :
		remove_local_sockets
		return 1
	}
	if ! printf '%s\n' "$_ls_out" |
	    grep -Fq 'new-remote-session (newr)'; then
		printf 'ERROR: local tmux has no new-remote-session command\n' >&2
		"$TMUX_BIN" -L "$_ls_probe" kill-server >/dev/null 2>&1 || :
		remove_local_sockets
		return 1
	fi
	"$TMUX_BIN" -L "$_ls_probe" kill-server >/dev/null 2>&1 || :
	remove_local_sockets
	return 0
}

ssh_exec()
{
	if [ -n "$REMOTE_PORT" ] && [ -n "$SSH_IDENTITY" ]; then
		exec ssh -o BatchMode=yes -o ConnectTimeout=10 \
		    -o ControlMaster=auto -o ControlPath="$PROBE_CTL" \
		    -o ControlPersist=yes -o ServerAliveInterval=15 \
		    -o ServerAliveCountMax=3 -p "$REMOTE_PORT" \
		    -i "$SSH_IDENTITY" "$REMOTE_HOST" "$@"
	elif [ -n "$REMOTE_PORT" ]; then
		exec ssh -o BatchMode=yes -o ConnectTimeout=10 \
		    -o ControlMaster=auto -o ControlPath="$PROBE_CTL" \
		    -o ControlPersist=yes -o ServerAliveInterval=15 \
		    -o ServerAliveCountMax=3 -p "$REMOTE_PORT" \
		    "$REMOTE_HOST" "$@"
	elif [ -n "$SSH_IDENTITY" ]; then
		exec ssh -o BatchMode=yes -o ConnectTimeout=10 \
		    -o ControlMaster=auto -o ControlPath="$PROBE_CTL" \
		    -o ControlPersist=yes -o ServerAliveInterval=15 \
		    -o ServerAliveCountMax=3 -i "$SSH_IDENTITY" \
		    "$REMOTE_HOST" "$@"
	fi
	exec ssh -o BatchMode=yes -o ConnectTimeout=10 \
	    -o ControlMaster=auto -o ControlPath="$PROBE_CTL" \
	    -o ControlPersist=yes -o ServerAliveInterval=15 \
	    -o ServerAliveCountMax=3 "$REMOTE_HOST" "$@"
}

# Run a POSIX sh command on the remote through the probe control master.
remote_cmd()
{
	_rc_inner=$1
	# /dev/null is important: cleanup runs this from a while loop reading
	# the list of remote names, and ssh would otherwise consume that list.
	timeout_cmd 25 ssh_exec "sh -c $(quote_sh "$_rc_inner")" </dev/null
}

remote_has_session()
{
	# A trailing colon makes the session target exact; session names cannot
	# contain a colon.  Some commands (show-options, capture-pane,
	# display-message) do not understand the "=name" prefix.
	remote_cmd "tmux has-session -t $(quote_sh "$1:") 2>/dev/null" \
	    >/dev/null 2>&1
}

remote_kill_session()
{
	remote_cmd "tmux kill-session -t $(quote_sh "$1:") 2>/dev/null" \
	    >/dev/null 2>&1 || :
}

remote_display()
{
	remote_cmd "tmux display-message -p -t $(quote_sh "$1:") \
	    $(quote_sh "$2")" 2>/dev/null
}

remote_show_option()
{
	remote_cmd "tmux show-options -t $(quote_sh "$1:") -v \
	    $(quote_sh "$2")" 2>/dev/null
}

remote_show_global_option()
{
	remote_cmd "tmux show-options -g -v $(quote_sh "$1")" 2>/dev/null
}

remote_show_global_env()
{
	_rge_out=$(remote_cmd \
	    "tmux show-environment -g $(quote_sh "$1") 2>/dev/null") ||
	    return 1
	case $_rge_out in
	-*) return 1 ;;
	*=*) printf '%s\n' "${_rge_out#*=}" ;;
	*) return 1 ;;
	esac
}

remote_show_session_env()
{
	_rse_out=$(remote_cmd "tmux show-environment -t \
	    $(quote_sh "$1:") $(quote_sh "$2") 2>/dev/null") || return 1
	case $_rse_out in
	-*) return 1 ;;
	*=*) printf '%s\n' "${_rse_out#*=}" ;;
	*) return 1 ;;
	esac
}

remote_list_windows()
{
	remote_cmd "tmux list-windows -t $(quote_sh "$1:") \
	    -F '#{window_name}'" 2>/dev/null
}

remote_capture()
{
	remote_cmd "tmux capture-pane -p -t $(quote_sh "$1:")" 2>/dev/null
}

remote_capture_has()
{
	remote_capture "$1" 2>/dev/null | grep -Fq "$2"
}

ssh_ctl_check()
{
	_scc_ctl=$1
	if [ -n "$REMOTE_PORT" ]; then
		exec ssh -o BatchMode=yes -o ConnectTimeout=5 \
		    -o ControlPath="$_scc_ctl" -O check -p "$REMOTE_PORT" \
		    "$REMOTE_HOST"
	fi
	exec ssh -o BatchMode=yes -o ConnectTimeout=5 \
	    -o ControlPath="$_scc_ctl" -O check "$REMOTE_HOST"
}

ssh_ctl_exit()
{
	_sce_ctl=$1
	if [ -n "$REMOTE_PORT" ]; then
		exec ssh -o BatchMode=yes -o ConnectTimeout=5 \
		    -o ControlPath="$_sce_ctl" -O exit -p "$REMOTE_PORT" \
		    "$REMOTE_HOST"
	fi
	exec ssh -o BatchMode=yes -o ConnectTimeout=5 \
	    -o ControlPath="$_sce_ctl" -O exit "$REMOTE_HOST"
}

ssh_ctl_true()
{
	_sct_ctl=$1
	if [ -n "$REMOTE_PORT" ]; then
		exec ssh -o BatchMode=yes -o ConnectTimeout=5 \
		    -o ControlMaster=no -o ControlPath="$_sct_ctl" \
		    -p "$REMOTE_PORT" "$REMOTE_HOST" true
	fi
	exec ssh -o BatchMode=yes -o ConnectTimeout=5 \
	    -o ControlMaster=no -o ControlPath="$_sct_ctl" \
	    "$REMOTE_HOST" true
}

ctl_check()
{
	timeout_cmd 8 ssh_ctl_check "$1" >/dev/null 2>&1
}

ctl_exit_socket()
{
	timeout_cmd 8 ssh_ctl_exit "$1" >/dev/null 2>&1
}

ctl_true()
{
	timeout_cmd 10 ssh_ctl_true "$1" >/dev/null 2>&1
}

make_root()
{
	if [ -n "$LOCAL_TEST_ROOT" ]; then
		case $LOCAL_TEST_ROOT in
		*' '*|*'	'*)
			printf 'ERROR: LOCAL_TEST_ROOT must not contain whitespace\n' >&2
			exit 2
			;;
		esac
		mkdir -p "$LOCAL_TEST_ROOT" || exit 1
		LOCAL_TEST_ROOT=$(CDPATH= cd "$LOCAL_TEST_ROOT" && pwd) ||
		    exit 1
		ROOT="$LOCAL_TEST_ROOT/run-$$-$SUFFIX"
		mkdir "$ROOT" || exit 1
	else
		ROOT=$(mktemp -d "${TMPDIR:-/tmp}/tmux-remote-ssh.XXXXXX") ||
		    exit 1
	fi
	RUNTIME=$(mktemp -d /tmp/tmux-rs-xdg.XXXXXX) || exit 1
	chmod 700 "$ROOT" "$RUNTIME" 2>/dev/null || :

	HOME_DIR="$ROOT/home"
	HOME=$HOME_DIR
	export HOME
	XDG_RUNTIME_DIR=$RUNTIME
	export XDG_RUNTIME_DIR
	PROBE_CTL="$RUNTIME/probe-$$.sock"

	mkdir -p "$HOME_DIR/.tmux/plugins/real-test" \
	    "$HOME_DIR/.config/tmux" "$ROOT/bin" "$ROOT/log" || exit 1

	# Keep the user's real SSH configuration and agent usable while using a
	# disposable HOME for ~/.tmux and ~/.tmux.conf.
	if [ -n "$REAL_HOME" ] && [ -d "$REAL_HOME/.ssh" ]; then
		ln -s "$REAL_HOME/.ssh" "$HOME_DIR/.ssh" || exit 1
	fi

	{
		printf '# Generated by regress/remote-ssh/run.sh\n'
		printf 'set -g @remote_real_test_marker "%s"\n' "$MARKER"
	} >"$HOME_DIR/.tmux.conf" || exit 1

	{
		printf '#!/bin/sh\n'
		printf '# Test plugin copied to the remote cache.\n'
		printf 'exit 0\n'
	} >"$HOME_DIR/.tmux/plugins/real-test/real-test.tmux" || exit 1
	chmod 755 "$HOME_DIR/.tmux/plugins/real-test/real-test.tmux" ||
	    exit 1
	printf '%s\n' "$MARKER" >"$HOME_DIR/.config/tmux/real-test-extra" ||
	    exit 1

	make_scripts
}

make_scripts()
{
	_q_bin=$(quote_sh "$TMUX_BIN")
	_q_sock=$(quote_sh "$SOCK")
	_q_host=$(quote_sh "$REMOTE_HOST")
	_q_port=
	if [ -n "$REMOTE_PORT" ]; then
		_q_port=$(quote_sh "$REMOTE_PORT")
	fi
	_q_id=
	if [ -n "$SSH_IDENTITY" ]; then
		_q_id=$(quote_sh "$SSH_IDENTITY")
	fi

	{
		printf '#!/bin/sh\n'
		printf 'unset TMUX TMUX_PANE\n'
		printf 'exec %s -L %s -f /dev/null new-remote-session -H %s' \
		    "$_q_bin" "$_q_sock" "$_q_host"
		if [ -n "$_q_port" ]; then
			printf ' -p %s' "$_q_port"
		fi
		if [ -n "$_q_id" ]; then
			printf ' -i %s' "$_q_id"
		fi
		printf ' "$@"\n'
	} >"$ROOT/bin/cli" || exit 1

	{
		printf '#!/bin/sh\n'
		printf 'unset TMUX TMUX_PANE\n'
		printf 'exec %s -L %s -f /dev/null attach-session -t "$1"\n' \
		    "$_q_bin" "$_q_sock"
	} >"$ROOT/bin/attach" || exit 1

	{
		printf '#!/bin/sh\n'
		printf 'exec %s -L %s -f /dev/null new-remote-session -H %s' \
		    "$_q_bin" "$_q_sock" "$_q_host"
		if [ -n "$_q_port" ]; then
			printf ' -p %s' "$_q_port"
		fi
		if [ -n "$_q_id" ]; then
			printf ' -i %s' "$_q_id"
		fi
		printf ' "$@"\n'
	} >"$ROOT/bin/inside" || exit 1

	chmod 755 "$ROOT/bin/cli" "$ROOT/bin/attach" "$ROOT/bin/inside" ||
	    exit 1
	sh -n "$ROOT/bin/cli" || exit 1
	sh -n "$ROOT/bin/attach" || exit 1
	sh -n "$ROOT/bin/inside" || exit 1
}

add_remote_name()
{
	printf '%s\n' "$1" >>"$ROOT/remote-sessions" || exit 1
}

local_has_session()
{
	tmux_main has-session -t "$1:" >/dev/null 2>&1
}

local_session_gone()
{
	! local_has_session "$1"
}

session_pane_id()
{
	tmux_main list-panes -t "$1:" -F '#{pane_id}' 2>/dev/null |
	    sed -n '1p'
}

session_pane_pid()
{
	tmux_main list-panes -t "$1:" -F '#{pane_pid}' 2>/dev/null |
	    sed -n '1p'
}

pane_value()
{
	tmux_main display-message -p -t "$1" "$2" 2>/dev/null || :
}

session_pane_dead()
{
	[ "$(pane_value "$1:0.0" '#{pane_dead}')" = 1 ]
}

session_pane_live()
{
	[ "$(pane_value "$1:0.0" '#{pane_dead}')" = 0 ]
}

pane_pid_differs()
{
	_ppd_new=$(session_pane_pid "$1")
	[ -n "$_ppd_new" ] && [ "$_ppd_new" != "$2" ]
}

client_count()
{
	tmux_main list-clients -F '#{client_session}' 2>/dev/null |
	    awk -v s="$1" '$0 == s { c++ } END { print c + 0 }'
}

client_at_least()
{
	[ "$(client_count "$1")" -ge "$2" ]
}

client_count_is()
{
	[ "$(client_count "$1")" -eq "$2" ]
}

capture_has()
{
	tmux_main capture-pane -p -t "$1" 2>/dev/null | grep -Fq "$2"
}

pane_start_command()
{
	tmux_main display-message -p -t "$1" '#{pane_start_command}' \
	    2>/dev/null || :
}

# Find the feature's SSH control socket from the pane's start command.  The
# helper is started with the control path as a positional argument, so it is
# visible without inspecting another process.
control_path_for_pane()
{
	_cpp_start=$(pane_start_command "$1")
	_cpp_path=$(printf '%s\n' "$_cpp_start" |
	    tr -s '[:space:]' '\n' |
	    while IFS= read -r _cpp_tok; do
		_cpp_tok=${_cpp_tok#\'}
		_cpp_tok=${_cpp_tok%\'}
		_cpp_tok=${_cpp_tok#\"}
		_cpp_tok=${_cpp_tok%\"}
		case $_cpp_tok in
		"$RUNTIME/tmux-remote-$UID_NUM/"*.sock)
			printf '%s\n' "$_cpp_tok"
			break
			;;
		esac
	    done)
	if [ -z "$_cpp_path" ]; then
		# Do not guess when more than one test socket exists.
		_cpp_count=0
		for _cpp_sock in \
		    "$RUNTIME"/tmux-remote-"$UID_NUM"/*.sock; do
			[ -S "$_cpp_sock" ] || continue
			_cpp_count=$((_cpp_count + 1))
			_cpp_path=$_cpp_sock
		done
		if [ "$_cpp_count" -ne 1 ]; then
			_cpp_path=
		fi
	fi
	printf '%s\n' "$_cpp_path"
}

ssh_master_pids()
{
	_smp_ctl=$1
	# With ControlPersist, OpenSSH forks a background mux master whose
	# command line is "ssh: /path [mux]".  The interactive ssh in the pane
	# is only a client of that process.  Stop both: the mux master makes the
	# liveness check hang and the client freezes the pane.
	ps -axo pid=,ppid=,command= 2>/dev/null |
	    awk -v ctl="$_smp_ctl" '
		{
			line = $0
			sub(/^[ \t]*[0-9]+[ \t]+[0-9]+[ \t]+/, "", line)
			if ((index(line, "ControlPath=" ctl) &&
			    index(line, "ControlMaster=auto")) ||
			    index(line, ctl " [mux]"))
				print $1
		}'
}

pid_alive()
{
	kill -0 "$1" 2>/dev/null
}

pid_gone()
{
	! kill -0 "$1" 2>/dev/null
}

outer_start()
{
	OUTER_SEQ=$((OUTER_SEQ + 1))
	LAST_OUTER="out$OUTER_SEQ"
	tmux_outer new-session -d -s "$LAST_OUTER" -x 120 -y 40 \
	    -- "$@" || return 1
	tmux_outer set-option -w -t "$LAST_OUTER" remain-on-exit on \
	    >/dev/null 2>&1 || :
	return 0
}

dump_outer()
{
	if [ -n "$LAST_OUTER" ]; then
		printf 'INFO: OUTER pane output (%s):\n' "$LAST_OUTER" >&2
		tmux_outer capture-pane -p -t "$LAST_OUTER" 2>/dev/null |
		    tail -n 20 >&2 || :
	fi
}

dump_local()
{
	printf 'INFO: local sessions and clients:\n' >&2
	tmux_main list-sessions -F \
	    '#{session_name} remote=#{session_remote} attached=#{session_attached}' \
	    2>/dev/null >&2 || :
	tmux_main list-clients -F '#{client_session}' 2>/dev/null >&2 || :
}

send_roundtrip()
{
	_sr_target=$1
	_sr_prefix=$2
	_sr_inner="printf '${_sr_prefix}%s\\n' \"\$((13*17))\""
	_sr_cmd="sh -c $(quote_sh "$_sr_inner")"
	tmux_main send-keys -t "$_sr_target" "$_sr_cmd" Enter \
	    2>/dev/null || return 1
}

send_and_wait_roundtrip()
{
	_sawr_target=$1
	_sawr_prefix=$2
	_sawr_expect="${_sawr_prefix}221"
	_sawr_try=0
	while [ "$_sawr_try" -lt 2 ]; do
		send_roundtrip "$_sawr_target" "$_sawr_prefix" || :
		if wait_for 20 "round trip through $_sawr_target" \
		    capture_has "$_sawr_target" "$_sawr_expect"; then
			return 0
		fi
		_sawr_try=$((_sawr_try + 1))
	done
	return 1
}

send_inside_start()
{
	_ssi_session=$1
	_ssi_cmd="$(quote_sh "$ROOT/bin/inside") -s $(quote_sh "$MODAL")"
	tmux_main send-keys -t "$_ssi_session:0.0" "$_ssi_cmd" Enter
}

send_inside_rerun()
{
	_ssr_session=$1
	_ssr_cmd="$(quote_sh "$ROOT/bin/inside") -s $(quote_sh "$MODAL"); printf 'RERUN_${SUFFIX}_%s\\n' \"\$((13*17))\""
	tmux_main send-keys -t "$_ssr_session:0.0" "$_ssr_cmd" Enter
}

modal_line()
{
	_ml_session=$1
	_ml_host=$2
	_ml_remote=$3
	tmux_main list-panes -t "$_ml_session:" -F \
	    '#{pane_id}|#{pane_remote}|#{pane_remote_host}|#{pane_remote_session}|#{pane_pid}|#{pane_width}|#{pane_height}|#{pane_modal_flag}' \
	    2>/dev/null |
	    awk -F'|' -v h="$_ml_host" -v s="$_ml_remote" \
	    '$2 == h && $4 == s { print; exit }'
}

modal_count()
{
	_mc_session=$1
	_mc_host=$2
	_mc_remote=$3
	tmux_main list-panes -t "$_mc_session:" -F \
	    '#{pane_remote}|#{pane_remote_session}' 2>/dev/null |
	    awk -F'|' -v h="$_mc_host" -v s="$_mc_remote" \
	    '$1 == h && $2 == s { c++ } END { print c + 0 }'
}

modal_field()
{
	printf '%s\n' "$1" | awk -F'|' -v n="$2" '{ print $n }'
}

# Continue any SSH master stopped by the hung-connection test.  Called both on
# the success path and from cleanup.
continue_stopped()
{
	for _cs_pid in $STOPPED_PIDS; do
		kill -s CONT "$_cs_pid" 2>/dev/null || :
	done
	STOPPED_PIDS=
}

cleanup()
{
	set +u
	# Never leave a SIGSTOPed ssh process behind.
	continue_stopped

	# Remove only the remote sessions named by this run.
	if [ -n "$REMOTE_HOST" ] && [ -n "$ROOT" ] &&
	    [ -f "$ROOT/remote-sessions" ]; then
		while IFS= read -r _cl_name; do
			[ -n "$_cl_name" ] || continue
			remote_kill_session "$_cl_name"
		done <"$ROOT/remote-sessions"
	fi

	# Close the test's probe master.
	if [ -n "$PROBE_CTL" ] && [ -n "$REMOTE_HOST" ] &&
	    [ -S "$PROBE_CTL" ]; then
		ctl_exit_socket "$PROBE_CTL" >/dev/null 2>&1 || :
	fi

	# Close the feature's control masters.  This only looks below the
	# private XDG_RUNTIME_DIR created for this run.
	if [ -n "$RUNTIME" ] && [ -d "$RUNTIME" ]; then
		for _cl_sock in "$RUNTIME"/tmux-remote-"$UID_NUM"/*.sock; do
			[ -S "$_cl_sock" ] || continue
			ctl_exit_socket "$_cl_sock" >/dev/null 2>&1 || :
		done
	fi

	_cl_servers_ok=1
	stop_server tmux_outer || _cl_servers_ok=0
	stop_server tmux_main || _cl_servers_ok=0
	if [ "$_cl_servers_ok" = 1 ]; then
		remove_local_sockets
	else
		printf 'INFO: a local test server did not exit; leaving its socket\n' >&2
	fi

	# Best-effort: terminate any still-running ssh process whose private
	# control path is in this run's runtime directory.  No other ssh
	# process matches this path.
	if [ -n "$RUNTIME" ]; then
		for _cl_pid in $(ps -axo pid=,command= 2>/dev/null |
		    awk -v pre="$RUNTIME/tmux-remote-$UID_NUM/" '
			index($0, "ControlPath=" pre) ||
			    index($0, "ssh: " pre) { print $1 }'); do
			kill -s TERM "$_cl_pid" 2>/dev/null || :
		done
	fi

	if [ -n "$ROOT" ] && [ -n "$RUNTIME" ]; then
		if [ "$KEEP_TEST_ROOT" = 1 ]; then
			printf 'INFO: kept local test files under %s and %s\n' \
			    "$ROOT" "$RUNTIME"
		else
			rm -rf "$ROOT" "$RUNTIME"
		fi
	fi
}

on_exit()
{
	_on_status=$?
	trap - 0
	cleanup
	exit "$_on_status"
}

detect_remote_server()
{
	_drs_rc=0
	_drs_out=$(remote_cmd \
	    "tmux list-sessions -F '#{session_name}' 2>/dev/null" \
	    2>/dev/null) || _drs_rc=$?
	if [ "$_drs_rc" -eq 0 ] && [ -n "$_drs_out" ]; then
		REMOTE_SERVER_PREEXISTING=1
	elif [ "$_drs_rc" -eq 0 ]; then
		# An empty server exits by itself, but be conservative if one
		# stays around.
		sleep 0.5
		if remote_cmd 'tmux list-sessions >/dev/null 2>&1' \
		    2>/dev/null; then
			REMOTE_SERVER_PREEXISTING=1
		else
			REMOTE_SERVER_PREEXISTING=0
		fi
	else
		REMOTE_SERVER_PREEXISTING=0
	fi
}

test_prerequisites()
{
	section "1. Prerequisites and versions"
	if local_sanity; then
		pass "local tmux provides new-remote-session (newr)"
	else
		fail "local tmux prerequisites failed"
		summary
		exit 1
	fi

	_pt_ver=
	_pt_rc=0
	_pt_ver=$(remote_cmd 'tmux -V' 2>&1) || _pt_rc=$?
	if [ "$_pt_rc" -ne 0 ]; then
		fail "remote SSH requires interactive input or is unreachable"
		printf 'INFO: remote command failed: %s\n' "$_pt_ver" >&2
		printf 'INFO: establish key/agent authentication first, for example:\n' >&2
		printf 'INFO:   ssh -o BatchMode=yes %s true\n' "$REMOTE_HOST" >&2
		summary
		exit 1
	fi
	pass "remote SSH reachable with BatchMode=yes"
	_pt_ver=$(printf '%s\n' "$_pt_ver" | tr -d '\r')
	# Keep only the version line in case the SSH wrapper or server emits
	# warnings on stderr.
	if printf '%s\n' "$_pt_ver" | grep -q '^tmux '; then
		_pt_ver=$(printf '%s\n' "$_pt_ver" |
		    grep '^tmux ' | sed -n '1p')
	fi
	note "remote tmux: $_pt_ver"
	assert_contains "remote tmux reports a version" "tmux " "$_pt_ver"

	if remote_cmd 'command -v tmux >/dev/null 2>&1 &&
	    command -v sh >/dev/null 2>&1 &&
	    command -v tar >/dev/null 2>&1 &&
	    command -v sed >/dev/null 2>&1 &&
	    command -v grep >/dev/null 2>&1'; then
		pass "remote has tmux, sh, tar, sed and grep"
	else
		fail "remote is missing tmux, sh, tar, sed or grep"
		summary
		exit 1
	fi

	detect_remote_server
	if [ "$REMOTE_SERVER_PREEXISTING" = 1 ]; then
		note "remote tmux server is already running; -f config loading cannot be checked"
	else
		note "remote tmux server will start fresh; config inheritance will be checked"
	fi
}

test_cli_create()
{
	section "2. CLI creation, attach and input/output round trip"
	if remote_has_session "$MAIN"; then
		fail "remote session $MAIN already exists"
		return
	fi
	add_remote_name "$MAIN"

	if ! outer_start "$ROOT/bin/cli" -s "$MAIN"; then
		fail "could not start the command under an OUTER tmux pty"
		return
	fi
	pass "command started under an OUTER tmux pty"

	if ! wait_for 60 "local session $MAIN" local_has_session "$MAIN"; then
		fail "local session $MAIN was not created"
		dump_outer
		dump_local
		return
	fi
	pass "local session $MAIN created"

	if wait_for 20 "attached client for $MAIN" \
	    client_at_least "$MAIN" 1; then
		pass "a local client is attached to $MAIN"
	else
		fail "no local client attached to $MAIN"
		dump_outer
	fi

	if ! wait_for 60 "remote session $MAIN" remote_has_session "$MAIN"; then
		fail "remote session $MAIN does not exist"
		dump_outer
		dump_local
		return
	fi
	pass "remote session $MAIN exists"
	MAIN_READY=1

	_ct_pid=$(session_pane_pid "$MAIN")
	if [ -n "$_ct_pid" ] && pid_alive "$_ct_pid"; then
		pass "local SSH pane process is alive (pid $_ct_pid)"
	else
		fail "local SSH pane process is not alive"
	fi

	_ct_id=$(session_pane_id "$MAIN")
	_ct_tag="RTRIP_${SUFFIX}_"
	if send_and_wait_roundtrip "$_ct_id" "$_ct_tag"; then
		pass "remote shell output reached the local pane"
	else
		fail "remote shell input/output round trip failed"
		printf 'INFO: local capture:\n' >&2
		tmux_main capture-pane -p -t "$_ct_id" 2>/dev/null |
		    tail -n 20 >&2 || :
		dump_outer
	fi

	_ct_ctl=$(control_path_for_pane "$_ct_id")
	if [ -n "$_ct_ctl" ]; then
		MAIN_CTL=$_ct_ctl
		pass "SSH control path located"
	else
		fail "could not locate the SSH control path"
	fi
}

test_metadata()
{
	section "3. Session metadata, formats and list-sessions"
	if [ "$MAIN_READY" != 1 ]; then
		skip "metadata checks need the session from test 2"
		return
	fi

	assert_eq "session @remote_host" "$REMOTE_HOST" \
	    "$(tmux_main show-options -t "$MAIN:" -v @remote_host)"
	assert_eq "session @remote_session" "$MAIN" \
	    "$(tmux_main show-options -t "$MAIN:" -v @remote_session)"

	_md_fmt=$(tmux_main display-message -p -t "$MAIN:" \
	    '#{session_remote}|#{session_remote_host}|#{session_remote_session}')
	assert_eq "session_remote formats" \
	    "$REMOTE_HOST|$REMOTE_HOST|$MAIN" "$_md_fmt"

	_md_sessions=$(tmux_main list-sessions -F \
	    '#{session_name}|#{session_remote}|#{session_remote_session}')
	assert_contains "list-sessions exposes remote metadata" \
	    "$MAIN|$REMOTE_HOST|$MAIN" "$_md_sessions"
	assert_contains "list-sessions shows the remote indicator" \
	    "[remote $REMOTE_HOST:$MAIN]" "$(tmux_main list-sessions)"

	_md_pane=$(session_pane_id "$MAIN")
	_md_pfmt=$(tmux_main display-message -p -t "$_md_pane" \
	    '#{pane_remote}|#{pane_remote_host}|#{pane_remote_session}')
	assert_eq "pane_remote formats" \
	    "$REMOTE_HOST|$REMOTE_HOST|$MAIN" "$_md_pfmt"
	assert_eq "pane @remote_host" "$REMOTE_HOST" \
	    "$(tmux_main show-options -p -t "$_md_pane" -v @remote_host)"
	assert_eq "pane @remote_session" "$MAIN" \
	    "$(tmux_main show-options -p -t "$_md_pane" -v @remote_session)"

	if [ -n "$REMOTE_PORT" ]; then
		assert_eq "session @remote_port" "$REMOTE_PORT" \
		    "$(tmux_main show-options -t "$MAIN:" -v @remote_port)"
		assert_eq "pane @remote_port" "$REMOTE_PORT" \
		    "$(tmux_main show-options -p -t "$_md_pane" -v @remote_port)"
	fi
}

test_control_master()
{
	section "4. Persistent SSH ControlMaster"
	if [ "$MAIN_READY" != 1 ]; then
		skip "ControlMaster checks need the session from test 2"
		return
	fi
	_cm_id=$(session_pane_id "$MAIN")
	_cm_ctl=$(control_path_for_pane "$_cm_id")
	if [ -z "$_cm_ctl" ]; then
		skip "could not locate the SSH control path"
		return
	fi
	MAIN_CTL=$_cm_ctl

	case $_cm_ctl in
	"$RUNTIME/tmux-remote-$UID_NUM/"*.sock)
		pass "control path is under XDG_RUNTIME_DIR"
		;;
	*)
		fail "control path is not under XDG_RUNTIME_DIR ($_cm_ctl)"
		;;
	esac
	if [ -S "$_cm_ctl" ]; then
		pass "control path is a Unix socket"
	else
		fail "control path is not a socket"
	fi

	assert_ok "ssh -O check succeeds" ctl_check "$_cm_ctl"
	assert_ok "multiplexed remote round trip succeeds" ctl_true "$_cm_ctl"

	_cm_start=$(pane_start_command "$_cm_id")
	assert_contains "helper uses ControlMaster=auto" \
	    "ControlMaster=auto" "$_cm_start"
	assert_contains "helper uses ControlPersist=yes" \
	    "ControlPersist=yes" "$_cm_start"
	assert_contains "helper uses ServerAliveInterval=15" \
	    "ServerAliveInterval=15" "$_cm_start"
	assert_contains "helper uses ServerAliveCountMax=3" \
	    "ServerAliveCountMax=3" "$_cm_start"
}

test_flags()
{
	section "5. Flag forwarding"
	if remote_has_session "$FLAGS"; then
		fail "remote session $FLAGS already exists"
		return
	fi
	add_remote_name "$FLAGS"

	_fw_win="rswin_${SUFFIX}"
	_fw_val="flagval_${SUFFIX}"
	_fw_cmd="printf 'RFLAG_%s\\n' \"\$TMUXRS_TEST\"; exec sleep 120"

	new_remote_main -d -s "$FLAGS" -c /tmp \
	    -e "TMUXRS_TEST=$_fw_val" -n "$_fw_win" \
	    -x 91 -y 27 -- sh -c "$_fw_cmd"
	if [ "$?" -ne 0 ]; then
		fail "new-remote-session rejected the flag test"
		return
	fi
	pass "new-remote-session accepted the flag test"
	pass "local -s creates local session $FLAGS"

	if ! wait_for 60 "remote session $FLAGS" remote_has_session "$FLAGS"; then
		fail "remote session $FLAGS was not created"
		dump_local
		return
	fi
	pass "remote -s creates remote session $FLAGS"

	_fw_pwd=$(remote_cmd 'cd /tmp && pwd -P' 2>/dev/null) || _fw_pwd=/tmp
	_fw_actual_pwd=$(remote_display "$FLAGS" '#{pane_current_path}')
	assert_eq "remote -c start directory" "$_fw_pwd" "$_fw_actual_pwd"

	_fw_env=$(remote_show_session_env "$FLAGS" TMUXRS_TEST) || _fw_env=
	assert_eq "remote -e environment" "$_fw_val" "$_fw_env"

	_fw_windows=$(remote_list_windows "$FLAGS")
	assert_eq "remote -n window name" "$_fw_win" \
	    "$(printf '%s\n' "$_fw_windows" | sed -n '1p')"

	_fw_size=$(remote_show_option "$FLAGS" default-size)
	assert_eq "remote -x/-y default-size" "91x27" "$_fw_size"

	if wait_for 20 "remote shell-command output" \
	    remote_capture_has "$FLAGS" "RFLAG_$_fw_val"; then
		pass "remote shell-command ran and received -e"
	else
		fail "remote shell-command output was not found"
	fi

	if [ "$(client_count "$FLAGS")" -eq 0 ]; then
		pass "local -d left the local session detached"
	else
		fail "local -d unexpectedly attached a client"
	fi
}

test_reuse()
{
	section "6. Attach/reuse of an existing session"
	if [ "$MAIN_READY" != 1 ]; then
		skip "reuse checks need the session from test 2"
		return
	fi
	_ru_pid=$(session_pane_pid "$MAIN")
	_ru_id=$(session_pane_id "$MAIN")
	_ru_ctl=$(control_path_for_pane "$_ru_id")
	if [ -z "$_ru_pid" ] || [ -z "$_ru_ctl" ]; then
		skip "could not identify the existing pane and control path"
		return
	fi

	if ! outer_start "$ROOT/bin/cli" -s "$MAIN"; then
		fail "could not start a second attaching client"
		return
	fi
	if ! wait_for 20 "second client attached to $MAIN" \
	    client_at_least "$MAIN" 2; then
		fail "second invocation did not attach a client"
		dump_outer
		return
	fi
	pass "second invocation attached the existing session"

	sleep 1
	_ru_pid2=$(session_pane_pid "$MAIN")
	_ru_id2=$(session_pane_id "$MAIN")
	_ru_ctl2=$(control_path_for_pane "$_ru_id2")
	assert_eq "no duplicate session was created" "1" \
	    "$(tmux_main list-sessions -F '#{session_name}' |
		awk -v s="$MAIN" '$0 == s { c++ } END { print c + 0 }')"
	assert_eq "the existing pane pid is unchanged" "$_ru_pid" "$_ru_pid2"
	assert_eq "the existing control path is unchanged" "$_ru_ctl" "$_ru_ctl2"
	assert_ok "reused control master is healthy" ctl_check "$_ru_ctl2"
	if remote_has_session "$MAIN"; then
		pass "remote session still exists after reuse"
	else
		fail "remote session disappeared after reuse"
	fi
}

test_config()
{
	section "7. Config and plugin inheritance"
	if [ "$MAIN_READY" != 1 ]; then
		skip "config checks need the session from test 2"
		return
	fi

	_cf_find="for d in \"\$HOME\"/.cache/tmux-remote/*; do
	    [ -f \"\$d/.tmux.conf\" ] || continue
	    if grep -q -F $(quote_sh "$MARKER") \"\$d/.tmux.conf\" 2>/dev/null
	    then printf '%s\\n' \"\$d\"; fi
	done"
	_cf_dirs=$(remote_cmd "$_cf_find" 2>/dev/null) || _cf_dirs=
	if [ -z "$_cf_dirs" ]; then
		fail "no copied config with the unique marker was found on the remote"
		return
	fi
	pass "remote cache contains the copied config marker"
	_cf_cache=$(printf '%s\n' "$_cf_dirs" | sed -n '1p')

	if remote_cmd "test -f $(quote_sh "$_cf_cache/.tmux/plugins/real-test/real-test.tmux")"; then
		pass "plugin file was copied to the remote cache"
	else
		fail "plugin file is missing from the remote cache"
	fi
	if remote_cmd "test -f $(quote_sh "$_cf_cache/.config/tmux/real-test-extra")"; then
		pass "~/.config/tmux was copied to the remote cache"
	else
		fail "~/.config/tmux contents are missing from the remote cache"
	fi

	if [ "$REMOTE_SERVER_PREEXISTING" = 1 ]; then
		skip "remote tmux server was already running; -f config loading is not testable"
		return
	fi

	_cf_marker=$(remote_show_global_option @remote_real_test_marker)
	assert_eq "remote tmux loaded the copied config marker" \
	    "$MARKER" "$_cf_marker"

	_cf_plugin=$(remote_show_global_env TMUX_PLUGIN_MANAGER_PATH) ||
	    _cf_plugin=
	if [ -z "$_cf_plugin" ]; then
		fail "remote TMUX_PLUGIN_MANAGER_PATH is not set"
	else
		pass "remote TMUX_PLUGIN_MANAGER_PATH is set"
		case $_cf_plugin in
		*/.cache/tmux-remote/*/.tmux/plugins)
			pass "TMUX_PLUGIN_MANAGER_PATH points into the remote cache"
			;;
		*)
			fail "TMUX_PLUGIN_MANAGER_PATH is not a cache plugin path ($_cf_plugin)"
			;;
		esac
		_cf_pcache=${_cf_plugin%/.tmux/plugins}
		if remote_cmd "test -f $(quote_sh "$_cf_pcache/.tmux/plugins/real-test/real-test.tmux")"; then
			pass "plugin file exists at TMUX_PLUGIN_MANAGER_PATH"
		else
			fail "plugin file is missing at TMUX_PLUGIN_MANAGER_PATH"
		fi
	fi

	_cf_term=$(remote_show_global_env TERM) || _cf_term=
	assert_eq "remote tmux TERM" "xterm-256color" "$_cf_term"
}

test_local_detach()
{
	section "8. Local detach survival"
	if [ "$MAIN_READY" != 1 ] || ! local_has_session "$MAIN"; then
		skip "local detach checks need the session from test 2"
		return
	fi
	if [ "$(client_count "$MAIN")" -lt 1 ]; then
		if ! outer_start "$ROOT/bin/attach" "$MAIN"; then
			fail "could not attach a client for the detach test"
			return
		fi
		if ! wait_for 20 "attached client for $MAIN" \
		    client_at_least "$MAIN" 1; then
			fail "could not attach a client for the detach test"
			return
		fi
	fi

	_ld_pid=$(session_pane_pid "$MAIN")
	_ld_id=$(session_pane_id "$MAIN")
	_ld_ctl=$(control_path_for_pane "$_ld_id")
	if [ -z "$_ld_pid" ]; then
		fail "could not read the SSH pane pid"
		return
	fi

	if ! tmux_main detach-client -s "$MAIN:" >/dev/null 2>&1; then
		fail "detach-client failed"
		return
	fi
	if ! wait_for 15 "all clients detached from $MAIN" \
	    client_count_is "$MAIN" 0; then
		fail "clients remained attached after detach-client"
	fi
	pass "clients detached from the local session"

	if local_has_session "$MAIN" && pid_alive "$_ld_pid" &&
	    session_pane_live "$MAIN"; then
		pass "SSH pane survived the local detach (pid $_ld_pid)"
	else
		fail "SSH pane did not survive the local detach"
	fi

	if [ -n "$_ld_ctl" ]; then
		assert_ok "control master is still healthy after local detach" \
		    ctl_check "$_ld_ctl"
	fi

	if ! outer_start "$ROOT/bin/attach" "$MAIN"; then
		fail "could not reattach after local detach"
		return
	fi
	if ! wait_for 20 "client reattached to $MAIN" \
	    client_at_least "$MAIN" 1; then
		fail "reattach did not attach a client"
		return
	fi
	pass "reattached after local detach"
	assert_eq "pane pid is unchanged after reattach" "$_ld_pid" \
	    "$(session_pane_pid "$MAIN")"
}

test_remote_detach()
{
	section "9. Remote detach and ControlMaster reuse"
	if [ "$MAIN_READY" != 1 ] || ! local_has_session "$MAIN"; then
		skip "remote detach checks need the session from test 2"
		return
	fi
	if [ "$(client_count "$MAIN")" -lt 1 ]; then
		if ! outer_start "$ROOT/bin/attach" "$MAIN"; then
			fail "could not attach a client for the remote detach test"
			return
		fi
		if ! wait_for 20 "attached client for $MAIN" \
		    client_at_least "$MAIN" 1; then
			fail "could not attach a client for the remote detach test"
			return
		fi
	fi

	_rd_pid=$(session_pane_pid "$MAIN")
	_rd_id=$(session_pane_id "$MAIN")
	# remain-on-exit is configurable; force it off here so this test checks
	# the default last-pane/session destruction deterministically.
	tmux_main set-option -p -t "$_rd_id" remain-on-exit off \
	    >/dev/null 2>&1 || :
	_rd_ctl=$(control_path_for_pane "$_rd_id")
	if [ -z "$_rd_ctl" ]; then
		skip "could not locate the control path for the remote detach test"
		return
	fi
	_rd_ino=$(ls -i "$_rd_ctl" 2>/dev/null | awk '{ print $1 }')

	assert_ok "control master is healthy before remote detach" \
	    ctl_check "$_rd_ctl"

	# End the remote tmux client without depending on its prefix binding.
	if remote_cmd "tmux detach-client -s $(quote_sh "$MAIN:") \
	    2>/dev/null"; then
		pass "remote tmux client detached"
	else
		fail "could not detach the remote tmux client"
		return
	fi

	if wait_for 20 "local session to disappear after remote detach" \
	    local_session_gone "$MAIN"; then
		pass "local session was destroyed with its last SSH pane"
	else
		fail "local session remained after its last SSH pane exited"
	fi

	if wait_for 12 "ControlMaster to persist after remote detach" \
	    ctl_check "$_rd_ctl"; then
		pass "ControlMaster persisted after remote detach"
	else
		fail "ControlMaster did not persist after remote detach"
	fi
	if remote_has_session "$MAIN"; then
		pass "remote tmux session persisted after detach"
	else
		fail "remote tmux session disappeared after detach"
	fi

	if ! outer_start "$ROOT/bin/cli" -s "$MAIN"; then
		fail "could not reattach after remote detach"
		return
	fi
	if ! wait_for 30 "local session recreated after remote detach" \
	    session_pane_live "$MAIN"; then
		fail "local session was not recreated after remote detach"
		return
	fi
	if ! wait_for 20 "client attached after remote detach" \
	    client_at_least "$MAIN" 1; then
		fail "no client attached after remote detach"
	fi
	pass "reattached after remote detach"

	_rd_pid2=$(session_pane_pid "$MAIN")
	_rd_ctl2=$(control_path_for_pane "$(session_pane_id "$MAIN")")
	assert_ne "reattach used a new pane process" "$_rd_pid" "$_rd_pid2"
	assert_eq "reattach used the same control path" "$_rd_ctl" "$_rd_ctl2"
	if wait_for 20 "healthy control master after reattach" \
	    ctl_check "$_rd_ctl"; then
		pass "control master is healthy after reattach"
	else
		fail "control master is not healthy after reattach"
	fi
	_rd_ino2=$(ls -i "$_rd_ctl" 2>/dev/null | awk '{ print $1 }')
	if [ -n "$_rd_ino" ] && [ -n "$_rd_ino2" ]; then
		assert_eq "control socket was reused (same inode)" \
		    "$_rd_ino" "$_rd_ino2"
	fi
	if remote_has_session "$MAIN"; then
		pass "remote session exists after reattach"
	else
		fail "remote session is missing after reattach"
	fi
	MAIN_LIVE=1
}

test_hung()
{
	section "10. Hung SSH reconnect"
	if [ "$MAIN_READY" != 1 ] || ! local_has_session "$MAIN"; then
		skip "hung SSH checks need the session from test 2"
		return
	fi
	if [ "$MAIN_LIVE" != 1 ] && ! session_pane_live "$MAIN"; then
		skip "hung SSH checks need a live pane"
		return
	fi
	_hg_pid=$(session_pane_pid "$MAIN")
	_hg_id=$(session_pane_id "$MAIN")
	_hg_ctl=$(control_path_for_pane "$_hg_id")
	if [ -z "$_hg_ctl" ] || ! command -v ps >/dev/null 2>&1; then
		skip "cannot identify the local SSH master for the hang simulation"
		return
	fi

	_hg_pids=$(ssh_master_pids "$_hg_ctl")
	if [ -z "$_hg_pids" ]; then
		skip "cannot find the local SSH master process for the hang simulation"
		return
	fi
	_hg_stopped=1
	for _hg_p in $_hg_pids; do
		if kill -s STOP "$_hg_p" 2>/dev/null; then
			STOPPED_PIDS="$STOPPED_PIDS $_hg_p"
		else
			_hg_stopped=0
		fi
	done
	if [ "$_hg_stopped" != 1 ]; then
		skip "could not SIGSTOP the local SSH master"
		continue_stopped
		return
	fi
	pass "SIGSTOPed the local SSH master (pids: $(printf '%s' \
	    "$_hg_pids" | tr '\n' ' '))"
	note "this is a best-effort simulation of a black-holed SSH connection"

	_hg_start=$(date +%s)
	if ! outer_start "$ROOT/bin/cli" -s "$MAIN"; then
		continue_stopped
		fail "could not start the attaching client"
		return
	fi

	if ! wait_for 15 "pane respawn after a hung liveness check" \
	    pane_pid_differs "$MAIN" "$_hg_pid"; then
		continue_stopped
		fail "attach-time liveness check did not respawn the hung pane"
		dump_outer
		return
	fi
	_hg_elapsed=$(($(date +%s) - _hg_start))
	pass "hung pane was respawned after ${_hg_elapsed}s"
	if [ "$_hg_elapsed" -lt 4 ]; then
		note "the liveness check failed quickly instead of timing out"
	fi
	assert_number_between "respawn happened within the expected time" \
	    "$_hg_elapsed" 0 15

	# The old master is stopped and cannot process the forced ssh -O exit
	# until it is continued.  Continue it now, then check that it ended.
	continue_stopped

	_hg_gone=1
	for _hg_p in $_hg_pids; do
		if ! wait_for 10 "old SSH master $_hg_p to exit" \
		    pid_gone "$_hg_p"; then
			_hg_gone=0
		fi
	done
	if [ "$_hg_gone" = 1 ]; then
		pass "old hung SSH connection was killed"
	else
		fail "old hung SSH connection is still running"
		for _hg_p in $_hg_pids; do
			kill -s TERM "$_hg_p" 2>/dev/null || :
		done
	fi

	if wait_for 30 "new pane to become live" session_pane_live "$MAIN"; then
		pass "new SSH pane is live"
	else
		fail "new SSH pane is not live"
	fi
	if wait_for 20 "new control master" ctl_check "$_hg_ctl"; then
		pass "new control connection is healthy"
	else
		fail "new control connection is not healthy"
	fi
	if wait_for 30 "remote session after reconnect" \
	    remote_has_session "$MAIN"; then
		pass "remote session still exists after reconnect"
	else
		fail "remote session disappeared after reconnect"
	fi

	_hg_tag="RHUNG_${SUFFIX}_"
	if send_and_wait_roundtrip "$(session_pane_id "$MAIN")" "$_hg_tag"; then
		pass "new connection passes an input/output round trip"
	else
		fail "new connection failed an input/output round trip"
	fi
	MAIN_LIVE=1
}

test_modal()
{
	section "11. Inside-tmux full-window modal pane"
	if remote_has_session "$MODAL"; then
		fail "remote session $MODAL already exists"
		return
	fi
	add_remote_name "$MODAL"

	if ! tmux_main new-session -d -s "$MODAL_SESSION" -x 120 -y 40 \
	    -- sh; then
		fail "could not create the inner test session"
		return
	fi
	if ! outer_start "$ROOT/bin/attach" "$MODAL_SESSION"; then
		fail "could not attach a pty client to the inner session"
		return
	fi
	if ! wait_for 20 "client attached to $MODAL_SESSION" \
	    client_at_least "$MODAL_SESSION" 1; then
		fail "no client attached to the inner test session"
		dump_outer
		return
	fi
	pass "inner session is attached through an OUTER pty"

	send_inside_start "$MODAL_SESSION" || {
		fail "could not send the inside-tmux command"
		return
	}
	if ! wait_for 60 "modal pane" \
	    modal_count_is "$MODAL_SESSION" "$REMOTE_HOST" "$MODAL" 1; then
		fail "modal pane was not created"
		dump_outer
		printf 'INFO: inner session panes:\n' >&2
		tmux_main list-panes -t "$MODAL_SESSION:" -F \
		    '#{pane_id}|#{pane_remote}|#{pane_remote_session}|#{pane_modal_flag}' \
		    2>/dev/null >&2 || :
		return
	fi
	pass "exactly one remote modal pane was created"

	_mo_line=$(modal_line "$MODAL_SESSION" "$REMOTE_HOST" "$MODAL")
	_mo_id=$(modal_field "$_mo_line" 1)
	_mo_host=$(modal_field "$_mo_line" 2)
	_mo_rhost=$(modal_field "$_mo_line" 3)
	_mo_rsession=$(modal_field "$_mo_line" 4)
	_mo_pid=$(modal_field "$_mo_line" 5)
	_mo_w=$(modal_field "$_mo_line" 6)
	_mo_h=$(modal_field "$_mo_line" 7)
	_mo_flag=$(modal_field "$_mo_line" 8)

	assert_eq "modal pane_remote" "$REMOTE_HOST" "$_mo_host"
	assert_eq "modal pane_remote_host" "$REMOTE_HOST" "$_mo_rhost"
	assert_eq "modal pane_remote_session" "$MODAL" "$_mo_rsession"
	assert_eq "modal pane is flagged as modal" "1" "$_mo_flag"
	assert_eq "window_modal_pane matches" "$_mo_id" \
	    "$(pane_value "$MODAL_SESSION:0" '#{window_modal_pane}')"

	if client_at_least "$MODAL_SESSION" 1 &&
	    ! local_has_session "$MODAL"; then
		pass "client stayed on its original session"
	else
		fail "client did not stay on its original session"
	fi

	_mo_ww=$(pane_value "$MODAL_SESSION:0" '#{window_width}')
	_mo_wh=$(pane_value "$MODAL_SESSION:0" '#{window_height}')
	case $_mo_ww:$_mo_wh:$_mo_w:$_mo_h in
	''|*[!0-9:]*)
		fail "modal size is not numeric"
		;;
	*)
		_mo_dw=$((_mo_ww - _mo_w))
		_mo_dh=$((_mo_wh - _mo_h))
		if [ "$_mo_dw" -ge 0 ] && [ "$_mo_dw" -le 4 ] &&
		    [ "$_mo_dh" -ge 0 ] && [ "$_mo_dh" -le 4 ]; then
			pass "modal pane covers the window (${_mo_w}x${_mo_h} of ${_mo_ww}x${_mo_wh})"
		else
			fail "modal pane size is not full-window (${_mo_w}x${_mo_h} of ${_mo_ww}x${_mo_wh})"
		fi
		;;
	esac

	_mo_tag="RMODAL_${SUFFIX}_"
	if send_and_wait_roundtrip "$_mo_id" "$_mo_tag"; then
		pass "modal pane input/output round trip succeeded"
	else
		fail "modal pane input/output round trip failed"
	fi

	if wait_for 30 "remote modal session" remote_has_session "$MODAL"; then
		pass "remote modal session exists"
	else
		fail "remote modal session is missing"
	fi

	# Run the command a second time from the original pane.  The trailing
	# printf proves the second command completed; its output is not present
	# in the typed command line.
	send_inside_rerun "$MODAL_SESSION" || {
		fail "could not send the second inside-tmux command"
		return
	}
	if ! wait_for 20 "second invocation to complete" capture_has \
	    "$MODAL_SESSION:0.0" "RERUN_${SUFFIX}_221"; then
		fail "second invocation did not complete"
		dump_outer
		return
	fi
	pass "second inside-tmux invocation completed"

	assert_eq "second invocation reused the modal pane id" \
	    "$_mo_id" \
	    "$(modal_field "$(modal_line "$MODAL_SESSION" "$REMOTE_HOST" "$MODAL")" 1)"
	assert_eq "second invocation reused the SSH pane pid" \
	    "$_mo_pid" \
	    "$(modal_field "$(modal_line "$MODAL_SESSION" "$REMOTE_HOST" "$MODAL")" 5)"
	assert_eq "second invocation did not create another modal pane" "1" \
	    "$(modal_count "$MODAL_SESSION" "$REMOTE_HOST" "$MODAL")"
	if client_at_least "$MODAL_SESSION" 1; then
		pass "client is still on its original session after reuse"
	else
		fail "client left its original session after reuse"
	fi
}

modal_count_is()
{
	[ "$(modal_count "$1" "$2" "$3")" -eq "$4" ]
}

resolve_tmux_bin()
{
	if [ -z "$TMUX_BIN" ]; then
		if [ -x "$REPO_ROOT/tmux" ]; then
			TMUX_BIN="$REPO_ROOT/tmux"
		else
			TMUX_BIN=tmux
		fi
	fi
	case $TMUX_BIN in
	*/*)
		_rtb_dir=$(dirname "$TMUX_BIN")
		_rtb_base=$(basename "$TMUX_BIN")
		TMUX_BIN=$(CDPATH= cd "$_rtb_dir" 2>/dev/null && pwd)/
		TMUX_BIN="$TMUX_BIN$_rtb_base"
		;;
	*)
		TMUX_BIN=$(command -v "$TMUX_BIN" 2>/dev/null || :)
		;;
	esac
	if [ -z "$TMUX_BIN" ] || [ ! -x "$TMUX_BIN" ]; then
		printf 'ERROR: cannot find an executable local tmux (TMUX_BIN=%s)\n' \
		    "${TMUX_BIN:-}" >&2
		exit 1
	fi
}

parse_args()
{
	CHECK_ONLY=0
	if [ "$#" -gt 0 ]; then
		case $1 in
		-h|--help)
			usage
			exit 0
			;;
		--check)
			CHECK_ONLY=1
			shift
			;;
		*)
			printf 'ERROR: unknown argument: %s\n' "$1" >&2
			usage >&2
			exit 2
			;;
		esac
	fi
	if [ "$#" -gt 0 ]; then
		printf 'ERROR: unexpected argument: %s\n' "$1" >&2
		usage >&2
		exit 2
	fi
}

main()
{
	parse_args "$@"
	resolve_tmux_bin

	if [ "$CHECK_ONLY" = 1 ]; then
		local_sanity || exit 1
		printf 'local checks passed; remote tests were not run\n'
		inventory
		exit 0
	fi

	if [ -z "$REMOTE_HOST" ]; then
		printf 'ERROR: REMOTE_HOST is required (for example user@server)\n' >&2
		usage >&2
		exit 2
	fi
	case $REMOTE_HOST in
	-*)
		printf 'ERROR: REMOTE_HOST must not start with -\n' >&2
		exit 2
		;;
	*' '*|*'	'*|*'|'*)
		printf 'ERROR: REMOTE_HOST must not contain whitespace or |\n' >&2
		exit 2
		;;
	esac
	if [ -n "$REMOTE_PORT" ]; then
		case $REMOTE_PORT in
		''|*[!0-9]*)
			printf 'ERROR: REMOTE_PORT must be numeric\n' >&2
			exit 2
			;;
		esac
		if [ "$REMOTE_PORT" -lt 1 ] || [ "$REMOTE_PORT" -gt 65535 ]; then
			printf 'ERROR: REMOTE_PORT must be between 1 and 65535\n' >&2
			exit 2
		fi
	fi
	case $KEEP_TEST_ROOT in
	0|1) ;;
	*)
		printf 'ERROR: KEEP_TEST_ROOT must be 0 or 1\n' >&2
		exit 2
		;;
	esac

	# The harness may itself be run from inside tmux.  Do not let that
	# influence the sessions and servers created here.
	unset TMUX TMUX_PANE
	# The harness uses its own pty servers; do not depend on the terminal
	# type of the shell that started it.  The remote helper overrides TERM
	# with xterm-256color for the remote tmux.
	TERM=screen
	export TERM

	SUFFIX="$$-$(date +%s)-$(random_suffix)"
	SOCK="tssh-main-$SUFFIX"
	OUTERSOCK="tssh-outer-$SUFFIX"
	MARKER="tmux-remote-real-$SUFFIX"
	MAIN="remote_ssh_main_$SUFFIX"
	FLAGS="remote_ssh_flags_$SUFFIX"
	MODAL="remote_ssh_modal_$SUFFIX"
	MODAL_SESSION="remote_ssh_shell_$SUFFIX"
	CHECK_ONLY=0

	make_root
	trap on_exit 0
	trap 'exit 130' 1
	trap 'exit 143' 15

	printf '== new-remote-session real SSH integration tests ==\n'
	printf 'local test root: %s\n' "$ROOT"
	printf 'remote host: %s%s\n' "$REMOTE_HOST" \
	    "${REMOTE_PORT:+ (port $REMOTE_PORT)}"
	inventory
	printf '\n'

	test_prerequisites

	if remote_has_session "$MAIN" ||
	    remote_has_session "$FLAGS" ||
	    remote_has_session "$MODAL"; then
		fatal "a unique remote test session name unexpectedly exists"
	fi

	test_cli_create
	test_metadata
	test_control_master
	test_flags
	test_reuse
	test_config
	test_local_detach
	test_remote_detach
	test_hung
	test_modal

	note "remote cache directories under ~/.cache/tmux-remote/ are left untouched"
	summary
	if [ "$FAILED" -ne 0 ]; then
		exit 1
	fi
	exit 0
}

main "$@"

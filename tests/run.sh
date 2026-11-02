#!/bin/sh
set -eu

command -v Xvfb >/dev/null || { echo "Tests require Xvfb" >&2; exit 1; }
testdir=$(mktemp -d)
xvfb_pid=
trap '[ -z "$xvfb_pid" ] || kill "$xvfb_pid" 2>/dev/null; rm -rf "$testdir"' EXIT
trap 'exit 1' HUP INT TERM

# -displayfd selects an unused display, separate from the user's session.
Xvfb -displayfd 3 -screen 0 1280x800x24 -nolisten tcp \
    3>"$testdir/display" >"$testdir/xvfb.log" 2>&1 &
xvfb_pid=$!
i=0
while [ ! -s "$testdir/display" ]; do
    i=$((i + 1))
    if [ "$i" -gt 100 ] || ! kill -0 "$xvfb_pid" 2>/dev/null; then
        tail -20 "$testdir/xvfb.log" >&2
        exit 1
    fi
    sleep 0.05
done
DISPLAY=":$(cat "$testdir/display")" "$1"

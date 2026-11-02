#!/bin/sh
# Try twmx in a nested Xephyr window without leaving your current session.
#
# usage: ./xephyr.sh [-s WIDTHxHEIGHT] [-2]
#   -s  size of the Xephyr window (default 1280x800; without -2 the nested
#       screen follows the window if your host WM resizes it)
#   -2  split the screen into two virtual monitors (multi-monitor testing)
#
# Click into the Xephyr window to give it the keyboard; Ctrl+Shift releases it.
# Super+Shift+Q (or closing the Xephyr window) ends the session.

size=1280x800
split=0
while getopts s:2 opt; do
	case $opt in
	s) size=$OPTARG ;;
	2) split=1 ;;
	*) sed -n '4,7p' "$0"; exit 1 ;;
	esac
done

cd "$(dirname "$0")" || exit 1
command -v Xephyr >/dev/null || { echo "Xephyr not found (Arch: xorg-server-xephyr)" >&2; exit 1; }
make -s || exit 1

# first free display number
d=1
while [ -e "/tmp/.X$d-lock" ] || [ -e "/tmp/.X11-unix/X$d" ]; do d=$((d + 1)); done

# resizeable: the nested screen follows the window size if your WM tiles it.
# Not with -2, where the virtual monitors are computed from the fixed size.
resizeable=-resizeable
[ $split = 1 ] && resizeable=
Xephyr ":$d" -br -ac -noreset $resizeable -screen "$size" 2>/dev/null &
xpid=$!
trap 'kill $xpid 2>/dev/null' EXIT INT TERM

# wait up to 5s for the server to accept connections
i=0
until DISPLAY=":$d" xprop -root >/dev/null 2>&1; do
	i=$((i + 1))
	[ $i -gt 50 ] && { echo "Xephyr did not start" >&2; exit 1; }
	kill -0 $xpid 2>/dev/null || { echo "Xephyr exited" >&2; exit 1; }
	sleep 0.1
done

export DISPLAY=":$d"
if [ $split = 1 ] && command -v xrandr >/dev/null; then
	w=${size%x*} h=${size#*x}
	out=$(xrandr | awk '/ connected/ {print $1; exit}')
	{
		xrandr --setmonitor left  "$((w / 2))/0x$h/0+0+0"          "${out:-none}"
		xrandr --setmonitor right "$((w - w / 2))/0x$h/0+$((w / 2))+0" none
	} >/dev/null 2>&1
fi

command -v xsetroot >/dev/null && xsetroot -solid '#222222'
command -v xterm >/dev/null && xterm &

echo "twmx running on $DISPLAY"
./twmx

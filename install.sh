#!/bin/sh
# twmx installer: downloads twmx from raw.githubusercontent.com, builds and
# installs it, and sets up a daily auto-updater (twmx-update).
#
#   curl -fsSL https://raw.githubusercontent.com/arcbaseproject/twmx/master/install.sh | sh
#   sh install.sh [options]
set -eu

usage() {
	cat <<'EOF'
usage: install.sh [options]
  --user            install for the current user into ~/.local (default)
  --system          install system-wide into /usr/local (run as root)
  --no-auto-update  do not set up the daily update timer
  --uninstall       remove twmx, twmx-update and the update timer
  -y, --yes         install missing packages without asking
EOF
}

URL=${TWMX_URL:-https://raw.githubusercontent.com/arcbaseproject/twmx/master}
mode=user autoupdate=1 action=install yes=0

say()  { printf '==> %s\n' "$*"; }
warn() { printf 'warning: %s\n' "$*" >&2; }
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }

for arg; do
	case $arg in
	--user) mode=user ;;
	--system) mode=system ;;
	--no-auto-update) autoupdate=0 ;;
	--uninstall) action=uninstall ;;
	-y|--yes) yes=1 ;;
	-h|--help) usage; exit 0 ;;
	*) usage >&2; die "unknown option: $arg" ;;
	esac
done

if [ $mode = system ]; then
	[ "$(id -u)" = 0 ] || die "--system needs root: sudo sh install.sh --system"
	PREFIX=/usr/local SRC=/usr/local/src/twmx
	UNITDIR=/etc/systemd/system
	systemctl_() { systemctl "$@"; }
else
	[ "$(id -u)" != 0 ] || die "run as your normal user, or pass --system"
	PREFIX=$HOME/.local SRC=$HOME/.local/src/twmx
	UNITDIR=${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user
	systemctl_() { systemctl --user "$@"; }
fi
XSESSION=/usr/share/xsessions/twmx.desktop

has() { command -v "$1" >/dev/null 2>&1; }

# Ask a yes/no question; works even when the script is piped into sh.
confirm() {
	[ $yes = 1 ] && return 0
	[ -r /dev/tty ] || return 1
	printf '%s [Y/n] ' "$1" >/dev/tty
	read -r ans </dev/tty || return 1
	case $ans in [nN]*) return 1 ;; esac
}

builddeps_ok() {
	has cc && has make && has pkg-config && pkg-config --exists x11 xrandr \
	&& { has curl || has wget; }
}

install_deps() {
	if has pacman; then
		cmd="pacman -S --needed" pkgs="base-devel curl libx11 libxrandr xterm dmenu"
	elif has apt-get; then
		cmd="apt-get install" pkgs="build-essential curl pkg-config libx11-dev libxrandr-dev xterm suckless-tools"
	elif has dnf; then
		cmd="dnf install" pkgs="gcc make curl pkgconf-pkg-config libX11-devel libXrandr-devel xterm dmenu"
	elif has zypper; then
		cmd="zypper install" pkgs="gcc make curl pkg-config libX11-devel libXrandr-devel xterm dmenu"
	elif has xbps-install; then
		cmd="xbps-install" pkgs="base-devel curl pkg-config libX11-devel libXrandr-devel xterm dmenu"
	else
		die "missing build tools; install a C compiler, make, curl, pkg-config and the libX11/libXrandr headers"
	fi
	[ "$(id -u)" = 0 ] || cmd="sudo $cmd"
	say "Missing build dependencies. Needed: $cmd $pkgs"
	confirm "Install them now?" || die "install the packages above and run this script again"
	# shellcheck disable=SC2086
	$cmd $pkgs
	builddeps_ok || die "build dependencies still missing"
}

download() {
	if has curl; then curl -fsSL --retry 2 -o "$2" "$1"; else wget -q -O "$2" "$1"; fi
}

install_twmx() {
	mkdir -p "$SRC"
	# carry over a customised config.h when run from a local checkout
	# (only when run as a file; when piped into sh, $0 says nothing)
	case $0 in */install.sh|install.sh) here=$(cd "$(dirname "$0")" && pwd) ;; *) here= ;; esac
	if [ -n "$here" ] && [ "$here" != "$SRC" ] && [ -f "$here/twmx.c" ] \
	&& [ -f "$here/config.h" ] && [ ! -f "$SRC/config.h" ]; then
		cp "$here/config.h" "$SRC/config.h"
		say "Copied your config.h from $here"
	fi
	# twmx-update does the download, staged build and install
	tmp=$(mktemp -d)
	trap 'rm -rf "$tmp"' EXIT
	say "Downloading twmx from $URL"
	download "$URL/twmx-update" "$tmp/twmx-update" || die "download failed (offline?)"
	TWMX_PREFIX=$PREFIX TWMX_SRC=$SRC TWMX_URL=$URL sh "$tmp/twmx-update" --force \
		|| die "installation failed; see the messages above"
	if [ $mode = system ]; then
		mkdir -p "$(dirname "$XSESSION")"
		printf '[Desktop Entry]\nName=twmx\nComment=Small tiling window manager\nExec=%s\nType=Application\n' \
			"$PREFIX/bin/twmx" >"$XSESSION"
		say "Added display manager session $XSESSION"
	fi
}

has_systemd() { has systemctl && [ -d /run/systemd/system ]; }

write_units() {
	mkdir -p "$UNITDIR"
	{
		echo "[Unit]"
		echo "Description=Update the twmx window manager from GitHub"
		[ $mode = system ] && printf 'Wants=network-online.target\nAfter=network-online.target\n'
		echo
		echo "[Service]"
		echo "Type=oneshot"
		echo "ExecStart=$PREFIX/bin/twmx-update"
	} >"$UNITDIR/twmx-update.service"
	cat >"$UNITDIR/twmx-update.timer" <<'EOF'
[Unit]
Description=Daily twmx update check

[Timer]
OnCalendar=daily
RandomizedDelaySec=1h
Persistent=true

[Install]
WantedBy=timers.target
EOF
}

setup_autoupdate() {
	if has_systemd; then
		write_units
		if systemctl_ daemon-reload && systemctl_ enable --now twmx-update.timer >/dev/null 2>&1; then
			say "Auto-update enabled (systemd timer twmx-update.timer, daily)"
			return
		fi
		warn "could not enable the systemd timer; falling back to cron"
	fi
	if has crontab; then
		{ crontab -l 2>/dev/null | grep -v '# twmx-update' || true
		  echo "@daily $PREFIX/bin/twmx-update >/dev/null 2>&1 # twmx-update"; } | crontab -
		say "Auto-update enabled (daily cron job)"
	else
		warn "no systemd or cron found; run twmx-update yourself to update"
	fi
}

remove_autoupdate() {
	if has_systemd && [ -f "$UNITDIR/twmx-update.timer" ]; then
		systemctl_ disable --now twmx-update.timer >/dev/null 2>&1 || true
		rm -f "$UNITDIR/twmx-update.timer" "$UNITDIR/twmx-update.service"
		systemctl_ daemon-reload || true
	fi
	if has crontab && crontab -l 2>/dev/null | grep -q '# twmx-update'; then
		crontab -l | grep -v '# twmx-update' | crontab -
	fi
}

uninstall() {
	remove_autoupdate
	rm -f "$PREFIX/bin/twmx" "$PREFIX/bin/twmx-update"
	[ $mode = system ] && rm -f "$XSESSION"
	say "Removed twmx from $PREFIX/bin"
	[ -d "$SRC" ] && say "Source and your config.h are still in $SRC (delete it if you don't need it)"
	return 0
}

hints() {
	echo
	say "twmx is installed: $PREFIX/bin/twmx"
	case ":$PATH:" in
	*":$PREFIX/bin:"*) ;;
	*) warn "$PREFIX/bin is not in your PATH; add it, or use the full path below" ;;
	esac
	echo "    Start it from ~/.xinitrc with:  exec $PREFIX/bin/twmx"
	[ $mode = system ] && echo "    or pick 'twmx' in your display manager."
	sudo=; [ $mode = system ] && sudo="sudo "
	echo "    Configure: edit $SRC/config.h, then run: ${sudo}twmx-update --force"
	has xterm || warn "xterm (default terminal, Super+Enter) is not installed"
	has dmenu_run || warn "dmenu (default launcher, Super+D) is not installed"
}

if [ $action = uninstall ]; then
	uninstall
	exit 0
fi
builddeps_ok || install_deps
install_twmx
if [ $autoupdate = 1 ]; then setup_autoupdate; else remove_autoupdate; fi
hints

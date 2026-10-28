#!/usr/bin/env bash
# Put your own pictures in the kid UI (personal use: themes stay on the watch, never in
# this repo). Run in Git Bash with the watch in Android:
#   theme-push.sh DIR      convert DIR (see make-theme.py) and install it
#   theme-push.sh --remove back to the built-in art
# The initramfs copies /data/linux/theme into Linux at each boot (adb reboot recovery).
set -euo pipefail
export MSYS_NO_PATHCONV=1
ADB=${ADB:-adb}
here=$(cd "$(dirname "$0")" && pwd)

if [ "${1:-}" = --remove ]; then
	"$ADB" shell 'rm -rf /data/linux/theme && sync' && echo "theme removed"
	exit 0
fi
[ $# -eq 1 ] && [ -d "$1" ] || { echo "usage: theme-push.sh DIR | --remove"; exit 2; }

wslpath() { wsl -d Ubuntu-24.04 -u builder -- wslpath -a "$(cygpath -m "$1")"; }
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
wsl -d Ubuntu-24.04 -u builder -- python3 "$(wslpath "$here/make-theme.py")" "$(wslpath "$1")" "$(wslpath "$out")"
"$ADB" shell 'rm -rf /data/linux/theme && mkdir -p /data/linux/theme'
"$ADB" push "$(cygpath -m "$out")/." /data/linux/theme/ > /dev/null	# adb.exe needs a Windows path
"$ADB" shell 'sync; ls /data/linux/theme | tr "\n" " "; echo'
echo "installed; it shows from the next Linux boot (adb reboot recovery)"

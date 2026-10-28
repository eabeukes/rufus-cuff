#!/usr/bin/env bash
# Boot recovery once, wait for Android to come back, save pstore + preloader timing.
#   try-recovery.sh <label>   -> logs/<label>/
set -euo pipefail
export MSYS_NO_PATHCONV=1
ADB=${ADB:-adb}
out=$(dirname "$0")/../logs/$1; mkdir -p "$out"
t0=$(date +%s); "$ADB" reboot recovery
"$ADB" wait-for-device
until [ "$("$ADB" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = 1 ]; do sleep 1; done
echo "back in Android after $(( $(date +%s) - t0 ))s"
for f in console-ramoops console-ramoops-2 pmsg-ramoops-0; do "$ADB" shell cat /sys/fs/pstore/$f > "$out/$f" 2>/dev/null || true; done
"$ADB" shell cat /proc/pl_lk > "$out/pl_lk.txt"
grep -a -h -E "RUFUS|Linux version|rufus-init" "$out/console-ramoops" "$out/console-ramoops-2" | head -20 || echo "no marker in console-ramoops or console-ramoops-2"

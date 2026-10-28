#!/usr/bin/env bash
# Reboot the watch and measure: kernel uptime at boot_completed, memory after a settle,
# and cold-start times (am start -W TotalTime, ms) for a few apps.
#   bench.sh <label>     -> appends one result block to bench/results.txt
set -euo pipefail
export MSYS_NO_PATHCONV=1  # Git Bash on Windows would rewrite /sys/... device paths
ADB=${ADB:-adb}
LABEL=${1:?label}
OUT=$(dirname "$0")/../bench; mkdir -p "$OUT"
APPS=(com.android.settings/.Settings com.android.dialer/.DialtactsWearActivity com.android.contacts/.activities.PeopleActivity)

"$ADB" reboot
"$ADB" wait-for-device
until [ "$("$ADB" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = 1 ]; do sleep 0.5; done
boot=$("$ADB" shell "cut -d' ' -f1 /proc/uptime" | tr -d '\r')

sleep 60  # let boot-time jobs settle
slab=$("$ADB" shell "grep -E '^(MemAvailable|Slab|SUnreclaim):' /proc/meminfo | tr -s ' ' | tr '\n' ' '" | tr -d '\r')
dbg=$("$ADB" shell cat /sys/kernel/slab/kmalloc-128/sanity_checks | tr -d '\r')

launches=""
for a in "${APPS[@]}"; do
  pkg=${a%%/*}; t=()
  for _ in 1 2 3; do
    "$ADB" shell am force-stop "$pkg"; sleep 2
    t+=("$("$ADB" shell am start -W -n "$a" | tr -d '\r' | awk '/TotalTime/{print $2}')")
    sleep 2; "$ADB" shell input keyevent KEYCODE_HOME
  done
  launches+="$pkg=${t[*]}  "
done

{
  echo "[$LABEL] $(date '+%F %T')  slab_sanity_checks=$dbg"
  echo "  boot_completed_uptime_s=$boot"
  echo "  $slab"
  echo "  cold_start_ms: $launches"
} | tee -a "$OUT/results.txt"

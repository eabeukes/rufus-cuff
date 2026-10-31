#!/bin/sh
# On the watch: let pupui suspend the watch by itself and log each sleep to /root/sleep-test.log:
# the time, how long it slept, and the SPM wake source r12 (bit 0 SPM timer, 2 power key,
# 5 CONN2AP, 6 RTC alarm). Charging is inhibited (pupui does not sleep on the charger) and USB
# is soft-disconnected; Windows needs a replug afterwards.
#   sleep-test.sh SECONDS    runs for SECONDS of uptime, which counts suspend
o=/root/sleep-test.log
u=/sys/class/udc/musb-hdrc.1.auto/soft_connect

# Each sleep's kernel line, with r12 as the SPM firmware left it (vendor "r12 backup").
drain() {
	dmesg -c | grep -o "slept [0-9]* ms, rc -*[0-9]*" | while read l; do
		echo "$(date +%T) $l r12 $(memcat 0x10006000 0x1000 | hexdump -s 0x608 -n 4 -e '"%08x"')" >> $o
	done
	sync
}

echo "== start $(date '+%F %T') boot $(cut -c1-8 /proc/sys/kernel/random/boot_id)" >> $o
echo 1 > /sys/module/mt6357_charger/parameters/inhibit
dmesg -c > /dev/null
sync; sleep 2; echo disconnect > $u
end=$(( $(cut -d. -f1 /proc/uptime) + $1 ))
while [ $(cut -d. -f1 /proc/uptime) -lt $end ]; do
	drain
	sleep 1
done
# The last sleep often runs past the end (the loop only gets to run while awake).
drain
echo connect > $u
echo 0 > /sys/module/mt6357_charger/parameters/inhibit
echo "== end $(date '+%F %T')" >> $o; sync

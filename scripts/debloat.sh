#!/usr/bin/env bash
# Re-apply the 2026-10-27 debloat to the Rufus Cuff over adb.
#   ./debloat.sh          disable packages, set animation scales
#   ./debloat.sh undo     re-enable them, reset animation scales
# Uses `pm disable-user`, never `pm uninstall --user 0` (Nougat can't undo that; see README).
# Never add com.wiite.freezeapp: the vendor framework checks tethering permissions as it.
set -euo pipefail

ADB=${ADB:-adb}
PACKAGES=(
  # vendor
  com.adups.fota com.adups.fota.sysoper com.mediatek.mtklogger com.baidu.map.location
  wiite.com.appbazaar cn.heils.faceunlock com.livephoto.live_wallpaper.realfire
  com.maxelus.colorslivewallpaper qs.wiite.cleantask com.wiite.wirteimei
  # Google
  com.google.android.googlequicksearchbox com.google.android.apps.maps com.android.vending
  com.google.android.tts com.google.android.backuptransport com.google.android.configupdater
  com.google.android.onetimeinitializer com.google.android.syncadapters.contacts
  com.google.android.gsf.login com.google.android.gms com.google.android.gsf
)

if [[ ${1:-} == undo ]]; then
  action=enable; scale=1
else
  action=disable-user; scale=0.5
fi

for p in "${PACKAGES[@]}"; do
  "$ADB" shell pm "$action" --user 0 "$p" | tr -d '\r'
done
for k in window_animation_scale transition_animation_scale animator_duration_scale; do
  "$ADB" shell settings put global "$k" "$scale"
done

# PackageManager writes package-restrictions.xml lazily; rebooting too soon loses the last changes.
echo "Done. Wait ~15 s before rebooting."

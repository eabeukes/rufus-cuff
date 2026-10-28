MediaTek CONNSYS firmware and this watch's Wi-Fi NVRAM (calibration, MAC),
copied from the watch's own /system/vendor/firmware and /nvdata. Not redistributable;
do not publish.

mt6739-bt-nvram.bin: Android's /data/nvram/APCFG/APRDEB/BT_Addr (66 bytes: this watch's
Bluetooth address and radio settings, which the vendor Bluetooth library sends to the chip at
start). btmt6739 sends the same commands at every Bluetooth power-on. Contains the device address.

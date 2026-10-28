# Sxmo device profile for the Rufus Cuff, found by its DT compatible "rufus,cuff" and
# sourced by /etc/profile.d/sxmo_init.sh. Buttons and rotation: /etc/sway/config.d/rufus.conf.
export SXMO_NO_MODEM=1		# no modem driver yet (LK leaves it off in recovery boot)
export SXMO_NO_AUDIO=1		# no audio driver yet
export SXMO_TOUCHSCREEN_ID="0:0:generic_ft5x06_(00)"
export SXMO_SWAY_SCALE=1
export SXMO_MIN_BRIGHTNESS=1
export SXMO_BEMENU_LANDSCAPE_LINES=8

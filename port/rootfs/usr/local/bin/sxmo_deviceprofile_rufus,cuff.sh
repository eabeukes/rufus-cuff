# Sxmo device profile for the Rufus Cuff, found by its DT compatible "rufus,cuff" and
# sourced by /etc/profile.d/sxmo_init.sh. Buttons and rotation: /etc/sway/config.d/rufus.conf.
export SXMO_NO_MODEM=1		# no modem driver yet (LK leaves it off in recovery boot)
export SXMO_NO_AUDIO=1		# no audio driver yet
export SXMO_TOUCHSCREEN_ID="0:0:generic_ft5x06_(00)"
export SXMO_SWAY_SCALE=1
export SXMO_MIN_BRIGHTNESS=1
export SXMO_BEMENU_LANDSCAPE_LINES=8
# Sxmo's bemenu defaults (sxmo_init.sh) with finger-sized rows: 14 pt text on 20 px lines is
# about 3 mm on this panel. 8 rows of 48 px plus the prompt fit the 480 px height.
export BEMENU_OPTS="--ab #222222 --af #bbbbbb --bdr #005577 --border 2 --cb #222222 --center --cf #bbbbbb --fb #222222 --fbb #eeeeee --fbf #222222 --ff #bbbbbb --fixed-height --fn 'Sxmo 22' --hb #005577 --hf #eeeeee --line-height 48 --margin 8 --nb #222222 --nf #bbbbbb --no-overlap --no-spacing --sb #323232 --scb #005577 --scf #eeeeee --scrollbar always --tb #005577 --tf #eeeeee --wrap"

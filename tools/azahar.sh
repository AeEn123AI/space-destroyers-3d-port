#!/bin/sh
# Azahar helper for headless testing on Xvfb.
#   tools/azahar.sh start <file.3dsx|.cia>   tools/azahar.sh stop
#   tools/azahar.sh key <xdotool key> [hold-seconds]   tools/azahar.sh shot out.png
export DISPLAY=${DISPLAY:-:99}
[ -n "$XAUTHORITY" ] || export XAUTHORITY=$(ps -eo args | grep -o "Xvfb $DISPLAY .*-auth [^ ]*" | head -1 | sed "s/.*-auth //")
case "$1" in
  start) flatpak run org.azahar_emu.Azahar "$(realpath "$2")" >/tmp/azahar.log 2>&1 &
         sleep 4; w=$(xdotool search --name "^Azahar [0-9]" | head -1); [ -n "$w" ] && xdotool windowmove $w 0 0 windowsize $w 1280 1000 ;;
  stop) pkill -9 -x azahar-launcher; pkill -9 -x azahar; sleep 1; pkill -f 'zenity.*Azahar' ;;
  key) xdotool keydown "$2"; sleep "${3:-0.15}"; xdotool keyup "$2" ;;
  shot) python3 "$(dirname "$0")/shot.py" "$2" ${3:+$3 $4 $5 $6} ;;
esac

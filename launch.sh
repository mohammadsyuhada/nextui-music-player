#!/bin/sh
DIR="$(dirname "$0")"
cd "$DIR"
export LD_LIBRARY_PATH="$DIR:$DIR/bins:$LD_LIBRARY_PATH"
# Set HOME so ALSA can find .asoundrc for Bluetooth/USB DAC audio routing
export HOME="/mnt/SDCARD/.userdata/tg5040"
"$DIR/musicplayer.elf" &> "$DIR/log.txt"

#!/bin/sh
DIR="$(dirname "$0")"
cd "$DIR"
export LD_LIBRARY_PATH="$DIR:$DIR/bins:$LD_LIBRARY_PATH"
"$DIR/musicplayer.elf" &> "$DIR/log.txt"

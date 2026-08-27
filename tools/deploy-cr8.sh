#!/bin/bash
# Replace the CR8 firmware safely: stop → verify offline → backup → swap → start.
# Usage: sudo ./deploy-cr8.sh <new.elf> [installed-name.elf]
set -u
NEW=${1:?usage: deploy-cr8.sh <new.elf> [installed-name.elf]}
NAME=${2:-$(basename "$NEW")}
DST=/lib/firmware/$NAME
HERE=$(cd "$(dirname "$0")" && pwd)
RP=/sys/class/remoteproc/remoteproc1

[ -f "$NEW" ] || { echo "no such file: $NEW"; exit 1; }

if [ "$(cat $RP/state)" != "offline" ]; then
    echo stop > $RP/state 2>/dev/null || true
    for i in $(seq 1 10); do [ "$(cat $RP/state)" = "offline" ] && break; sleep 1; done
fi
[ "$(cat $RP/state)" = "offline" ] || { echo "R8 won't stop"; exit 1; }

[ -f "$DST" ] && cp "$DST" "$DST.bak-$(date +%Y%m%d-%H%M%S)"
cp "$NEW" "$DST"; chmod 644 "$DST"
echo "  installed $DST ($(md5sum "$DST" | cut -d" " -f1))"
exec "$HERE/start-cr8.sh" "$NAME"

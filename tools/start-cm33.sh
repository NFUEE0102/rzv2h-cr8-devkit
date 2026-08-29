#!/bin/bash
# Start the CM33 (remoteproc0) with the required pre-start sequence.
# Usage: sudo ./start-cm33.sh <firmware-name.elf> [tries]
#
# ☠ Read docs/11 before stopping the CM33: on THIS kernel build, `stop`
#   memsets the CM33's 63MB carveout window — which contains core0's entire
#   resource set and core1's rsctbl. Start the CM33 last, avoid stopping it.
set -u
FW=${1:?usage: start-cm33.sh <fw.elf> [tries]}
TRIES=${2:-3}
RP=/sys/class/remoteproc/remoteproc0

start_once () {
    if [ "$(cat $RP/state)" != "offline" ]; then
        echo "  CM33 already $(cat $RP/state) — refusing to stop it (docs/11 mine #4)."
        echo "  If you really need a restart: stop it manually, then fully restart BOTH R8 cores."
        return 1
    fi
    busybox devmem 0x104800A8 32 1   # clear stale MHU msg ch5 doorbell (CA55->CM33)
    busybox devmem 0x42F00044 8 0    # virtio status, rsctbl@42f00000 slice 0
    busybox devmem 0x42F000B4 8 0    # slice 1
    echo "$FW" > $RP/firmware
    timeout 25 sh -c "echo start > $RP/state" || return 1
    sleep 2
    [ "$(cat $RP/state)" = "running" ]
}

for n in $(seq 1 "$TRIES"); do
    if start_once; then
        echo "  CM33 running ($FW)"
        exit 0
    fi
    [ "$(cat $RP/state)" != "offline" ] && exit 1   # running-refusal is final
    echo "  attempt $n failed — retrying"
done
echo "  FAILED after $TRIES attempts; dmesg tail:"; dmesg | tail -6
exit 1

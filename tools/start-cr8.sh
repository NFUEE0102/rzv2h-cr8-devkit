#!/bin/bash
# Start the CR8 core0 firmware with the required pre-start sequence.
# Usage: sudo ./start-cr8.sh <firmware-name.elf> [tries]
# The firmware must already be in /lib/firmware and phdr-patched (see docs/03).
set -u
FW=${1:?usage: start-cr8.sh <fw.elf> [tries]}
TRIES=${2:-4}
RP=/sys/class/remoteproc/remoteproc1
BB=0x431F0000        # black box (fault_type at +0x44) — optional but used if present

start_once () {
    # Must truly be offline before touching `firmware`: writing it while running
    # returns EBUSY but state still says "running" — the OLD image keeps executing.
    if [ "$(cat $RP/state)" != "offline" ]; then
        echo stop > $RP/state 2>/dev/null || true
        for i in $(seq 1 10); do [ "$(cat $RP/state)" = "offline" ] && break; sleep 1; done
    fi
    [ "$(cat $RP/state)" = "offline" ] || { echo "  R8 won't stop (state=$(cat $RP/state))"; return 1; }

    busybox devmem 0x10480068 32 1   # clear stale MHU ch3 doorbell
    busybox devmem 0x42F00044 8 0    # virtio status, rsctbl slice 0
    busybox devmem 0x42F000B4 8 0    # virtio status, slice 1 (both are required)
    echo "$FW" > $RP/firmware
    echo start > $RP/state
    sleep 3
    [ "$(cat $RP/state)" = "running" ] || return 1
    # If the firmware carries the black box, verify it did not die at boot.
    F=$(busybox devmem $((BB + 0x44)) 32 2>/dev/null || echo 0x00000000)
    [ "$F" = "0x00000000" ]
}

for n in $(seq 1 "$TRIES"); do
    if start_once; then
        [ "$n" -gt 1 ] && echo "  R8 running ($FW), attempt $n" || echo "  R8 running ($FW)"
        exit 0
    fi
    echo "  attempt $n failed (fault=$(busybox devmem $((BB+0x44)) 32 2>/dev/null)) — retrying"
done
echo "  FAILED after $TRIES attempts; dmesg tail:"; dmesg | tail -5
exit 1

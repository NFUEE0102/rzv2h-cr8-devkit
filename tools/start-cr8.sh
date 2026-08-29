#!/bin/bash
# Start the CR8 core0 firmware with the required pre-start sequence.
# Usage: sudo ./start-cr8.sh <firmware-name.elf> [tries]
# The firmware must already be in /lib/firmware and phdr-patched (see docs/03).
#
# Layout note (2026-08-29): firmware built before the per-core carveout
# migration keeps its resource table at 0x42F00000 (the region the DT assigns
# to the CM33) with the black box at 0x431F0000; per-core builds use
# 0x42F02000 (rsctbl-cr8-0) and 0x439F0000.  This script services both:
# it clears the virtio status bytes of BOTH resource tables and accepts the
# boot-health check from either black-box location.
set -u
FW=${1:?usage: start-cr8.sh <fw.elf> [tries]}
TRIES=${2:-4}
RP=/sys/class/remoteproc/remoteproc1
BB_SHARED=0x431F0000    # black box, shared-layout firmware (fault_type at +0x44)
BB_PERCORE=0x439F0000   # black box, per-core-layout firmware

# A location is healthy when it holds no captured fault: 0 = explicitly clean,
# 0xFFFFFFFF = cold DDR / no black-box firmware has run there.
bb_ok () {
    local v
    v=$(busybox devmem $(($1 + 0x44)) 32 2>/dev/null || echo 0x00000000)
    [ "$v" = "0x00000000" ] || [ "$v" = "0xFFFFFFFF" ]
}

start_once () {
    # Must truly be offline before touching `firmware`: writing it while running
    # returns EBUSY but state still says "running" — the OLD image keeps executing.
    if [ "$(cat $RP/state)" != "offline" ]; then
        echo stop > $RP/state 2>/dev/null || true
        for i in $(seq 1 10); do [ "$(cat $RP/state)" = "offline" ] && break; sleep 1; done
    fi
    [ "$(cat $RP/state)" = "offline" ] || { echo "  R8 won't stop (state=$(cat $RP/state))"; return 1; }

    busybox devmem 0x10480068 32 1   # clear stale MHU ch3 doorbell
    busybox devmem 0x42F00044 8 0    # virtio status, shared-layout rsctbl slice 0
    busybox devmem 0x42F000B4 8 0    # virtio status, shared-layout slice 1
    busybox devmem 0x42F02044 8 0    # virtio status, per-core rsctbl (cr8-0) slice 0
    busybox devmem 0x42F020B4 8 0    # virtio status, per-core slice 1
    echo "$FW" > $RP/firmware
    echo start > $RP/state
    sleep 3
    [ "$(cat $RP/state)" = "running" ] || return 1
    # If the firmware carries the black box, verify it did not die at boot.
    # Whichever layout the firmware uses, the OTHER location holds cold or
    # last-healthy data, so requiring both to be fault-free is safe.
    bb_ok $BB_SHARED && bb_ok $BB_PERCORE
}

for n in $(seq 1 "$TRIES"); do
    if start_once; then
        [ "$n" -gt 1 ] && echo "  R8 running ($FW), attempt $n" || echo "  R8 running ($FW)"
        exit 0
    fi
    echo "  attempt $n failed (fault shared=$(busybox devmem $((BB_SHARED+0x44)) 32 2>/dev/null) percore=$(busybox devmem $((BB_PERCORE+0x44)) 32 2>/dev/null)) — retrying"
done
echo "  FAILED after $TRIES attempts; dmesg tail:"; dmesg | tail -5
exit 1

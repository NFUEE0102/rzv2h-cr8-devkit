#!/bin/bash
# Start the CR8 core1 firmware (remoteproc2) with the required pre-start sequence.
# Usage: sudo ./start-cr8-core1.sh <firmware-name.elf> [tries]
# core1 layout: rsctbl 0x42F04000, vrings 0x44000000 (8MB), black box 0x441F0000,
# MHU msg ch4 (doorbell clear 0x10480088; core0's equivalent is ch3 / 0x10480068).
set -u
FW=${1:?usage: start-cr8-core1.sh <fw.elf> [tries]}
TRIES=${2:-4}
RP=/sys/class/remoteproc/remoteproc2
BB=0x441F0000        # core1 black box (fault_type at +0x44)

bb_ok () {
    local v
    v=$(busybox devmem $(($1 + 0x44)) 32 2>/dev/null || echo 0x00000000)
    [ "$v" = "0x00000000" ] || [ "$v" = "0xFFFFFFFF" ]
}

start_once () {
    if [ "$(cat $RP/state)" != "offline" ]; then
        echo stop > $RP/state 2>/dev/null || true
        for i in $(seq 1 10); do [ "$(cat $RP/state)" = "offline" ] && break; sleep 1; done
    fi
    [ "$(cat $RP/state)" = "offline" ] || { echo "  core1 won't stop (state=$(cat $RP/state))"; return 1; }

    busybox devmem 0x10480088 32 1   # clear stale MHU msg ch4 doorbell (CA55->CR8_1)
    busybox devmem 0x42F04044 8 0    # virtio status, rsctbl-cr8-1 slice 0
    busybox devmem 0x42F040B4 8 0    # virtio status, slice 1 (both are required)
    echo "$FW" > $RP/firmware
    echo start > $RP/state
    sleep 3
    [ "$(cat $RP/state)" = "running" ] || return 1
    bb_ok $BB
}

# 安全守門(對稱版,見 start-cr8.sh / docs/10):
OTHER_STATE=$(cat /sys/class/remoteproc/remoteproc1/state 2>/dev/null || echo none)
if [ "$OTHER_STATE" = "running" ] && [ "${FORCE:-0}" != "1" ]; then
    if [ "$(cat $RP/state)" != "offline" ] || \
       [ "$(busybox devmem $BB 32 2>/dev/null)" = "0x42423852" ]; then
        echo "  ✗ cr8-core0 running 且本核曾跑過 —— 單核重啟不安全(無 per-core reset)。"
        echo "    請用 tools/restart-r8-pair.sh(兩核全下再依序上)。"
        exit 1
    fi
fi

for n in $(seq 1 "$TRIES"); do
    if start_once; then
        [ "$n" -gt 1 ] && echo "  CR8 core1 running ($FW), attempt $n" || echo "  CR8 core1 running ($FW)"
        exit 0
    fi
    echo "  attempt $n failed (fault=$(busybox devmem $((BB+0x44)) 32 2>/dev/null)) — retrying"
done
echo "  FAILED after $TRIES attempts; dmesg tail:"; dmesg | tail -6
exit 1

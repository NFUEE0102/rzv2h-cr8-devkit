#!/bin/bash
# Restore the RZ/V2H EVK CR8-DevKit SD image onto a target SD card.
#
# Usage:  sudo CONFIRM=YES ./restore-evk-sd.sh /dev/sdX
#
# Needs a card >= 256 GB (partition table is restored verbatim).
# The card's entire contents are DESTROYED.
#
# Order matters (lesson learned the hard way on this platform):
#   1. raw first-4MiB  — RZ/V2H boots from RAW sectors below partition 1:
#      BL2 + FIP + U-Boot live in 0..4MiB. A backup that only covers the
#      partitions produces a card that does not boot at all.
#      (The raw blob includes an MBR; sfdisk overwrites it next, which is fine.)
#   2. sfdisk partition table
#   3. p1 (FAT boot files)
#   4. p2 (rootfs, e2image raw stream; conv=sparse writes only used blocks)
set -euo pipefail

DEV=${1:?usage: sudo CONFIRM=YES $0 /dev/sdX}
HERE=$(cd "$(dirname "$0")" && pwd)

[ "$(id -u)" = 0 ] || { echo "run as root"; exit 1; }
[ -b "$DEV" ] || { echo "$DEV is not a block device"; exit 1; }
case "$DEV" in *[0-9]) echo "give the whole disk (e.g. /dev/sdb), not a partition"; exit 1;; esac
[ "${CONFIRM:-}" = "YES" ] || { echo "This DESTROYS $DEV. Re-run with CONFIRM=YES."; exit 1; }

for f in bootloader-raw-4MiB.img.gz parttable.txt p1-boot.img.gz p2-rootfs.e2img.gz SHA256SUMS; do
    [ -f "$HERE/$f" ] || { echo "missing $f"; exit 1; }
done
echo "== verifying checksums =="
( cd "$HERE" && sha256sum -c SHA256SUMS )

# partition suffix: /dev/sdb -> sdb1 ; /dev/mmcblk0 -> mmcblk0p1
P=""
case "$DEV" in *mmcblk*|*nvme*) P="p";; esac

echo "== 1/4 raw bootloader region (first 4 MiB) =="
zcat "$HERE/bootloader-raw-4MiB.img.gz" | dd of="$DEV" bs=1M conv=fsync status=progress

echo "== 2/4 partition table =="
sfdisk "$DEV" < "$HERE/parttable.txt"
partprobe "$DEV"; sleep 2

echo "== 3/4 p1 (boot FAT) =="
zcat "$HERE/p1-boot.img.gz" | dd of="${DEV}${P}1" bs=4M conv=fsync status=progress

echo "== 4/4 p2 (rootfs) — sparse write, ~29 GB of real data =="
zcat "$HERE/p2-rootfs.e2img.gz" | dd of="${DEV}${P}2" bs=4M conv=sparse,fsync status=progress
e2fsck -fy "${DEV}${P}2" || true    # journal replay from the live capture

sync
echo "== done. Boot it, then verify per docs/05: =="
echo "   remoteproc1 present; /dev/uio0..9; tools/start-cr8.sh <fw>; bb.py clean"

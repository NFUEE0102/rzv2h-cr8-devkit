# RZ/V2H CR8-DevKit — EVK reference SD image (16 GB-card edition)

The reference image is a fully configured EVK ver1 system, stripped of
factory AI demo assets (~22 GB removed) so it **flashes onto any 16 GB (or
larger) SD card**. Burn it and the board boots straight into the full CR8 stack
described in [rzv2h-cr8-devkit](https://github.com/NFUEE0102/rzv2h-cr8-devkit).

## What's inside

- Linux `6.10.14-arm64-renesas` (#3) with built-in CR8 remoteproc
- DTB with the RSCI5 handover applied (both copies; pre-handover backups kept)
- `cr8-uio-bind.service` enabled → `/dev/uio0..9` at boot
- libmetal / libopen_amp with `dmb sy` fences
- `/lib/firmware/cr8_sensors.elf` (v26) + rpmsg echo demo ELF
- `~/r8_bench` client (sources + binaries, `make` works on-board) and
  `~/r8web` tooling incl. the black-box reader `bb.py`
- Recovery layer on the card itself: factory kernel at
  `/boot/Image.factory-0611`, factory modules at
  `/lib/modules/6.10.14-arm64-renesas.factory`
- Ubuntu userland intact (apt, python3, gcc) — only demo *data* was removed
  (`test0528`, `sample_yolox_cam`, DRP-AI benches, WFB assets, logs)

Credentials: `ubuntu` / `ubuntu`. SSH enabled.

## Flash

`evk-devkit-16gb-v1.0.1.img.xz` — balenaEtcher flashes the `.img.xz` directly;
or:

```sh
xz -dc evk-devkit-16gb-v1.0.1.img.xz | sudo dd of=/dev/sdX bs=4M conv=fsync status=progress
```

Geometry: 12.2 GiB total (p1 FAT 200 MB, p2 ext4 12 GiB, ~4 GB free).
(8 GB cards are not feasible: ext4's shrink floor for this filesystem is
8.11 GiB — inode tables and fragmentation, not data, set the limit.)

## First boot

1. Board must be an RZ/V2H **EVK ver1** (boot blobs + DTB are board-specific).
2. Optional — grow the rootfs to fill a larger card:

```sh
echo ", +" | sudo sfdisk -N 2 --no-reread /dev/mmcblk0 && sudo partprobe
sudo resize2fs /dev/mmcblk0p2
```

3. Acceptance test (devkit `docs/05-blackbox.md`):

```sh
ls /sys/class/remoteproc/      # remoteproc1 = cr8-core0
ls /dev/uio*                   # uio0..uio9
sudo /usr/local/sbin/... # or: devkit tools/start-cr8.sh cr8_rpmsg_echo_demo.elf
cd ~/r8_bench && sudo R8_BENCH_NOTS=1 ./r8_bench 1000
sudo python3 ~/r8web/bb.py
```

## Files

| File | Content |
|---|---|
| `evk-devkit-16gb-v1.0.1.img.xz` | the flashable image (this is all you need) |
| `bootloader-raw-4MiB.img.gz` | raw sectors 0–4 MiB (MBR+BL2+FIP) — kept for manual recovery; **already inside the .img** |
| `parttable.txt`, `p1-boot.img.gz`, `p2-rootfs.e2img.gz` | capture pieces + `restore-evk-sd.sh` (alternative restore path) |
| `SHA256SUMS` | checksums |

Captured from a live rootfs after `sync`; assembly ran `e2fsck` so the journal
is clean in the image.

## v1.0.1 changes

- On-board `start-r8-sensors.sh` / `start-demo.sh`: peripheral-ownership check
  now reads the DT node status (`/sys/firmware/devicetree/base/soc/serial@12802000/status`)
  instead of testing `/dev/ttySC1` existence — the ttySC numbering shifts with
  the enabled-serial set and misfired on the EVK. The getting-started warning
  about this no longer applies to this image.

# 01 — Boot & kernel

## What the kernel must provide

CR8 support is **built into the kernel image** (no modules involved):

- `rz-rproc` driver probing `renesas,rzv2h-cr8` (upstream Multi-OS 4.1 feature,
  backported to the 6.10 product line in our build)
- `uio` + `uio_pdrv_genirq` built in (the rpmsg path binds UIO by
  `driver_override`, not by compatible matching — see doc 04)

Check on a running board:

```sh
ls /sys/class/remoteproc/          # want remoteproc1 = cr8-core0
cat /sys/class/remoteproc/remoteproc1/name
```

If only `remoteproc0` (cm33) exists, your kernel predates the CR8 backport.

## Porting the stack to a fresh board (how the EVK was done)

The EVK shipped with a factory kernel *without* CR8 support but with a DTB that
**already contains** every CR8 node (device nodes, reserved-memory, UIO nodes).
That made the port a kernel swap plus one DT status flip:

1. **Pin down the boot chain before touching anything.** Verify `uEnv.txt` is
   live: `/proc/cmdline` must match `mmc_args` in `/boot/uEnv.txt` word for word.
   Then you know exactly which `Image` and which `.dtb` file U-Boot loads.
2. **Back up**: `sudo cp /boot/Image /boot/Image.factory` — recovery is
   "pull the SD card, copy the backup over `/boot/Image` on partition 2".
3. **Install the CR8-capable kernel** as `/boot/Image`, and replace
   `/lib/modules/<version>/` wholesale (move the factory directory aside first —
   same version string does not mean same build; never merge the two).
4. **Apply the DT handover** (doc 02) and reboot.
5. Verify: three remoteprocs probed, ten `/dev/uio*` after enabling the bind
   service, then run the echo test from the README quickstart.

Gotchas that cost us time:

- `echo x | sudo -S tee /sys/...` feeds the *password* to `tee`. For sysfs writes
  always use `echo <pw> | sudo -S sh -c "echo x > /sys/..."`.
- Boards have no SSH trust between each other; relay files with `scp -3` through
  your workstation.
- A cold board's DDR is uninitialized: the black box reads `0xFFFFFFFF` until the
  firmware first runs, and the rpmsg resource table is written by the Linux
  client's priming pass — both are normal.

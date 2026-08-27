# 02 — Device tree

`dts/rzv2h-evk-ver1-cr8-handover.dtb` is the EVK ver1 factory DTB with the one
required change already applied. Install it as **both** DTB files (they must stay
identical):

```sh
sudo cp dts/rzv2h-evk-ver1-cr8-handover.dtb /boot/dtb/renesas/rzv2h-evk-ver1.dtb
sudo cp dts/rzv2h-evk-ver1-cr8-handover.dtb /boot/r9a09g057h44-rzv2h-evk.dtb
```

## What the CR8 stack needs in the DT (all present in stock RDK/EVK DTBs)

**Remoteproc device nodes** (`compatible = "renesas,rzv2h-cr8"`):

```
/soc/cr8-core0   reg = TCM 0x12040000/0x40000, SRAM 0x08180000/0x80000,
                        DDR 0x40800000/0x1000000;  renesas,rz-rsctbl = 0x42f02000
/soc/cr8-core1   (mirrored at the next windows)
```

**Reserved-memory carveouts**: `vdev0vring0/1@0x43000000/0x43050000`,
`vdev0buffer@0x43200000`, `vring0/1@43800000/43900000`, `buffer@43a00000`
(+ the core1 mirrors at `0x44xxxxxx`), `rsctbl-cr8-0/1@42f02000/42f04000`.

**Custom UIO nodes** (root level; these are what the Linux client actually uses —
the doorbell and shared memory are exposed to userspace, *not* to a kernel
mailbox/rpmsg driver):

| Node | compatible | Region |
|---|---|---|
| `mbox-uio@10480000` (+3 `receiver@…` children) | `mbox_uio` | MHU doorbell |
| `rsctbl@42f00000`, `rsctbl-cr8-0/1@…` | `shm_uio` | resource tables |
| `mhu-shm@42f01000` | `shm_uio` | MHU scratch |
| `vring-ctl0/1@43000000/43100000` | `vring_uio` | vring descriptors (1 MB each) |
| `vring-shm0/1@43200000/43500000` | `shm_uio` | buffers (3 MB each) |

No kernel driver matches these compatibles — binding happens at boot via
`driver_override` (see `tools/cr8-uio-bind.sh`, doc 04).

## The peripheral handover (the only edit)

Any peripheral the R8 firmware owns must be `disabled` on the Linux side.
Otherwise Linux's runtime PM gates the module clock and the R8's first register
access takes an imprecise bus error — which surfaces as a baffling async Data
Abort somewhere else entirely.

For the sensor firmware that means RSCI5 (GPS UART). One line, no DT rebuild:

```sh
sudo fdtput -t s <your.dtb> /soc/serial@12802000 status disabled
```

(RSCI7 / I2C nodes at `…@12802800` are already disabled in factory DTBs.)

⚠️ Do **not** use `/dev/ttySC*` names to check who owns a port — the index shifts
with the set of enabled serials (on the EVK, `ttySC1` is RSCI4, not RSCI5).
Check the DT directly:

```sh
tr -d '\0' < /sys/firmware/devicetree/base/soc/serial@12802000/status
```

⚠️ When inspecting the DT, use `/sys/firmware/devicetree/base` — `find` on
`/proc/device-tree` silently returns nothing (it's a symlink `find` won't follow).

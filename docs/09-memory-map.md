# 09 — Three-core memory map

Every core owns its own slice of memory. Stick to the table below and the
three cores coexist cleanly; borrow another core's addresses and they will
corrupt each other.

## The map

| Resource | CM33 | CR8 core0 | CR8 core1 |
|---|---|---|---|
| remoteproc | remoteproc0 | remoteproc1 | remoteproc2 |
| program SRAM | 0x08000000 (vector 0x08003000) | 0x08180000 | 0x08200000 |
| TCM (AXI view) | — | 0x12040000 | 0x12080000 |
| DDR window | 0x40010000 | 0x40800000 | 0x41800000 |
| resource table | 0x42F00000 | 0x42F02000 | 0x42F04000 |
| vrings ch0/ch1 | 0x43000000 / 0x43050000 | 0x43800000 / 0x43900000 | 0x44000000 / 0x44100000 |
| shm buffers | 0x43200000 | 0x43A00000 / 0x43D00000 | 0x44200000 / 0x44500000 |
| black box | 0x431F0000 | 0x439F0000 | 0x441F0000 |
| MHU msg/rsp ch | 5 / 8 (clear 0x104800A8) | 3 / 6 (clear 0x10480068) | 4 / 7 (clear 0x10480088) |
| MHU receiver (UIO) | receiver@10480100 | receiver@104800C0 | receiver@104800E0 |
| tick timer | OSTM/GTM ch0 | GTM ch4 | GTM ch5 |

Shared by design: the MHU shmem page at `0x42F01000` (per-channel slots) and
the MHU register block (`mbox-uio@10480000`).

## UIO node names (what a Linux client opens)

| | CR8 core0 | CR8 core1 | CM33 |
|---|---|---|---|
| rsctbl | `42f02000.rsctbl-cr8-0` | `42f04000.rsctbl-cr8-1` | `42f00000.rsctbl` |
| vring ctl0/ctl1 | `43800000.vring-ctl0-c0` / `43900000.vring-ctl1-c0` | `44000000.vring-ctl0-c1` / `44100000.vring-ctl1-c1` | `43000000.vring-ctl0` / `43100000.vring-ctl1` |
| shm 0/1 | `43a00000.vring-shm0-c0` / `43d00000.vring-shm1-c0` | `44200000.vring-shm0-c1` / `44500000.vring-shm1-c1` | `43200000.vring-shm0` / `43500000.vring-shm1` |

All 20 UIO devices appear at boot (`cr8-uio-bind.service`); clients look them
up **by name**, never by `/dev/uioN` number. `tools/percore-board.sh` applies
the DT nodes and bind patterns to a board that does not have them yet.

## Rules

1. **A firmware must link against its own column.** The load-bearing spots in
   a firmware port: the linker script origins, `OpenAMP_RPMsg_cfg.h` (six
   addresses + four UIO names), **`rsc_table.h` `CFG_RSCTBL_MEM_PA`** (the R8
   polls this at runtime), the MPU table, and the black-box address. The
   matching Linux client carries the same six addresses/names plus the rsctbl
   name/PA and its `MBX_ID`.
2. **The CM33's DT `reg` must stay the three-window split** (`cm33_sram` +
   `cm33_rsctbl` 0x42F00000/0x2000 + `cm33_vring` 0x43000000/0x600000).
   Stopping a core makes the kernel zero that core's registered carveouts —
   with the split, the CM33 only ever clears its own memory.
3. **One client per channel.** Each core's rpmsg link supports exactly one
   resident Linux client at a time; a second client corrupts the shared
   vrings. Stop the web daemons before running `r8_bench*` by hand.
4. The legacy pair — `cr8_demo_patched.elf` + the `r8_bench.shared-layout`
   binary — uses the CM33 column's addresses on core0. Keep them together and
   never mix them with the current per-core firmware/clients.

## Verifying a layout

```sh
sudo python3 ~/r8web/bb.py --all      # one line per core, from its own black box
sudo busybox devmem 0x43000000 32     # CM33 vring region: must stay untouched
                                      # by the R8 cores (0xFFFFFFFF when cold)
```

After any firmware rebuild, confirm the generated files actually match the
configuration: check `rzv_gen/vector_data.c` (e.g. `GTM5 INT` for core1) —
the XML alone does not prove the build regenerated.

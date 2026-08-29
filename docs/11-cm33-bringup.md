# 11 — CM33 bring-up (all three cores live)

2026-08-29, same night as docs/10: **cm33 + cr8-core0 + cr8-core1 all running,
three rpmsg links under simultaneous full traffic** (CM33 echo 2000/2000 and
core1 echo 2000/2000 concurrently, while core0 served the PWM web demo).

## The archived mystery, resolved

CM33 bring-up had failed three times in July (hard-froze the whole machine,
pre-UART) and was archived. The July verdict already pinned it: the same
firmware boots fine on the factory 6.10 kernel — the freeze is a **6.18
(rz-cmn-3.3) kernel regression**. This kit's image runs a 6.10 base, and on it
the CM33 started **on the first attempt**: `echo start >
/sys/class/remoteproc/remoteproc0/state` → `remote processor cm33 is now up`.
No bootaddr changes, no clock-order patches — none of the July workarounds were
needed (or ever were the problem). Do **not** start the CM33 from a 6.18-based
kernel.

## Layout (the factory shared region — legally CM33's)

| Item | Value |
|---|---|
| remoteproc | remoteproc0 |
| vector table | 0x08003000 (= DT `renesas,rz-bootaddrs`), code in CM33 SRAM 0x08000000 |
| resource table | 0x42F00000 (`rsctbl@42f00000`, single vdev) |
| vrings / buffer | 0x43000000/0x43050000 / 0x43200000 |
| MHU msg/rsp ch | 5 / 8 (doorbell clear **0x104800A8**), receiver@10480100 |
| tick timer | GTM/OSTM ch0 @0x11800000 (the one Linux leaves disabled) |

This only became possible after docs/09: core0 used to squat on exactly these
addresses.

## Linux side

This kernel has no `virtio_rpmsg_bus`, so the kernel registers `virtio0` and
stops there — the userspace OpenAMP client takes the host role, same as for the
R8 cores. Client = `examples/linux-client` with the **shared-layout** addresses
(the pre-docs/09 values), `MBX_ID = UIO_RECEIVER1`, and **single-channel mode**:

> **The CM33 demo firmware carries one vdev (svc0) only.** The client's
> CR8-style "prime ch0, work on ch1" flow waits forever on a service the CM33
> never announces. Use `platform_init(0,0,...)` directly and bind
> `rpmsg-service-0`. (Built on the board as `r8_bench_cm33`.)

Start sequence (`tools/start-cm33.sh`): clear MHU ch5 doorbell + both virtio
status bytes of `0x42F00000`, write firmware name, start.

## ☠ Mine #4 — stopping the CM33 wipes the other cores

`rzv2h_stop_cm33()` in this rz_rproc build memsets **every registered CM33
carveout** on stop — and the CM33's DT `reg` window registers
**0x40010000+0x3EF0000 (63 MB)** as one big carveout. That range contains
core0's heap/rsctbl/vrings/black box (0x408xxxxx/0x42F02xxx/0x438-0x439xxxxx)
and core1's rsctbl + the shared MHU shmem (0x42F04000/0x42F01000). Net effect:
**`echo stop > remoteproc0/state` silently lobotomizes core0 and cripples
core1's rpmsg.** (core1's vrings at 0x44xxxxxx survive — outside the window.)

Rules until the driver/DT is fixed:
1. **Start the CM33 last; avoid stopping it.** Its `start` is harmless — only
   `stop` (or a failed start's cleanup) runs the wipe.
2. If the CM33 was stopped: fully restart both R8 cores afterwards — **both
   down, then core0 up, then core1 up** (a single-core restart with the other
   core running has shown a stage-0 freeze; the full down/up sequence is
   verified clean) — then reconnect all clients.
3. Root fix (backlog): patch `rz_rproc.c` to skip the `reg`-window carveouts in
   the stop-path memset (or shrink the CM33 DT window and add explicit
   reserved-memory nodes), then rebuild the kernel.

## Order of bring-up for all three cores

```sh
sudo ~/devkit/tools/start-cr8.sh cr8_pwm_demo.elf         # core0
sudo ~/devkit/tools/start-cr8-core1.sh cr8_core1_demo.elf # core1
sudo ~/devkit/tools/start-cm33.sh cm33_demo.elf           # cm33 — last
```

## CM33 black box (all three cores observable)

The CM33 firmware in this kit carries a Cortex-M33 port of the black box at
**phys `0x431F0000`** (CM33 view `0x831F0000` — the old core0 location, inside
the CM33's own region). Key offsets match the R8 layout (`+0x40` stage,
`+0x44` fault, `+0x154` tick) so the start scripts' boot-health check works
unchanged; the magic differs (`BB33`) so tools can tell the core type. On a
HardFault it captures CFSR/HFSR/BFAR/MMFAR plus the stacked PC/LR — the
"pre-UART freeze with zero information" failure mode of the July attempts can
never happen silently again. Sources: `blackbox/cm33_blackbox.{c,h}`, hooks in
`hal_entry.c` (init), `blinky_thread_entry.c` (pump), `main_task_entry.c`
(stage 12 on vdev-ready — it stays at 10 until a client connects).

One command now shows every core:

```sh
sudo python3 ~/r8web/bb.py --all
#   cr8-core0  boot#1  uptime  50700  stage 12  [R8]
#   cr8-core1  boot#4  uptime  48000  stage 12  [R8]
#   cm33       boot#1  uptime  57000  stage 12  [CM33]
```

`--addr 0x431F0000` gives the CM33 detail view. To swap CM33 firmware safely
(mine #4: its stop wipes the other cores), use `tools/redeploy-cm33.sh
<new.elf>` — it performs the stop → install → start → full R8 rebuild sequence.

## Origin of mine #4, verified

The stop-path `memset` of registered carveouts is **stock Renesas rz-cmn
behaviour**, not ours (our July patch only reshaped the CR8 cluster logic; the
memset shows as context in that diff). For the CR8 cores the DT `reg` windows
are cleanly partitioned per core, so wiping "your own" carveouts is sound —
the defect is solely the CM33 node's greedy 63 MB `reg` window predating the
three-core world. Root fix (backlog): skip `reg`-window carveouts in the stop
memset (~5 lines in `rz_rproc.c`) and rebuild the kernel.

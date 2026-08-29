# RZ/V2H Cortex-R8 DevKit

Everything needed to bring up, deploy to, talk to, and **prove the health of** the
Cortex-R8 real-time core on Renesas RZ/V2H boards (RDK and EVK), running FreeRTOS
alongside Linux on the CA55.

This is a staging deliverable of ongoing work on a fixed-wing UAV onboard computer.
It packages the pieces that took real debugging to get right: the device tree, the
firmware deployment pipeline, the rpmsg communication path, a DDR "black box" for
acceptance testing on a console-less core, and the FSP-level fixes we needed along
the way.

Verified on: RZ/V2H RDK (WS125-V2HRDKREFZ) and RZ/V2H EVK ver1, Linux
`6.10.14-arm64-renesas`, FSP 3.1.0, FreeRTOS 10.6.1.

## Layout

| Path | What it is |
|---|---|
| `docs/00-getting-started.md` | **Start here** — first time with the board: flash, boot, run the R8, build your own firmware |
| `docs/01-boot-and-kernel.md` | Kernel requirements; porting the stack to a fresh board (EVK walkthrough) |
| `docs/02-device-tree.md` | Every DT node the CR8 stack needs, and the one-line peripheral handover |
| `docs/03-deploy-firmware.md` | How to replace the R8 firmware — including the mandatory ELF phdr fix |
| `docs/04-communication.md` | How CA55 ↔ CR8 rpmsg actually works here (UIO + libmetal, no kernel rpmsg) |
| `docs/05-blackbox.md` | The black box: field map, reader, and acceptance criteria |
| `docs/07-pwm-web-demo.md` | Write your own firmware: web knobs -> rpmsg -> GPT hardware PWM |
| `docs/08-evk-pmod-pinout.md` | EVK Pmod headers: which pin is physically where (CN1/CN2/CN3/CN6) |
| `docs/09-percore-migration.md` | CR8_0 moved to its per-core carveout — the CM33 region is free now |
| `docs/10-core1-bringup.md` | Both R8 cores running side by side — layout table + the three mines |
| `docs/11-cm33-bringup.md` | CM33 up too — all three cores live; the carveout-wipe mine |
| `docs/12-core1-uart-demo.md` | core1 UART terminal on the web page; mine #5 (no single-core R8 restart) |
| `blackbox/` | `r8_blackbox.c/.h` (firmware side) + `bb.py` (Linux-side reader) |
| `tools/` | `start-cr8.sh`, `deploy-cr8.sh`, `patch-elf-phdr.py`, UIO bind service |
| `dts/` | EVK DTB with the CR8 handover applied, ready to install |
| `fsp-patches/` | FSP 3.1.0 files with our fixes (lr_svc save, vector-table bounds check) |
| `examples/linux-client/` | Full CA55 client source (`r8_bench`) — builds on the board with the provided Makefile |
| `examples/firmware/` | Prebuilt rpmsg echo firmware (phdr-fixed, ready to load) |

## Fastest path: flash the reference image

A ready-to-flash EVK SD image (16 GB cards and up) with everything below
pre-installed is on the [Releases page](https://github.com/NFUEE0102/rzv2h-cr8-devkit/releases)
— then follow **`docs/00-getting-started.md`** step by step: it assumes no
prior remoteproc/FSP knowledge and goes from flashing the card to building and
deploying your own firmware. The rest of this README is the condensed version
for people who already know the stack.

## Quickstart (board already prepared per docs/01 and docs/02)

```sh
# 1. Install the UIO bind service (once)
sudo cp tools/cr8-uio-bind.sh /usr/local/sbin/
sudo cp tools/cr8-uio-bind.service /etc/systemd/system/
sudo systemctl enable --now cr8-uio-bind.service
ls /dev/uio*        # expect uio0..uio9

# 2. Load and start the echo firmware
sudo cp examples/firmware/cr8_rpmsg_echo_demo.elf /lib/firmware/
sudo tools/start-cr8.sh cr8_rpmsg_echo_demo.elf

# 3. Build and run the client
cd examples/linux-client && make
sudo R8_BENCH_NOTS=1 ./r8_bench 1000
#   expect: 1000/1000 ok, 0 errors, p99.9 well under 0.5 ms
```

## Acceptance testing with the black box

Any firmware that links `blackbox/r8_blackbox.c` exposes its health at physical
address `0x431F0000`, readable from Linux with no console and no rpmsg dependency:

```sh
sudo python3 blackbox/bb.py
```

Pass criteria (see `docs/05-blackbox.md` for the full field map):

- `死因/fault: none` and boot stage reaches the application stage
- `tick` advances continuously (scheduler alive, independent of any task)
- heartbeats advance; `sem_give == sem_take` (no lost wakeups)
- zero `notif_err` / `send_err`; nesting events may occur — with the FSP fixes
  applied they are harmless

## The FSP fixes (fsp-patches/)

Two defects in the stock FSP 3.1.0 CR8 support, found the hard way and fixed here.
Both files are drop-in replacements for `rzv/fsp/src/...` in an e2 studio project
(they also carry optional, weak-linked diagnostic hooks that are no-ops unless the
black box is present):

1. **`portASM.asm` — `lr_svc` not saved across the IRQ C-handler calls.**
   Upstream FreeRTOS brackets the handler call with `PUSH {…, LR}` / `POP {…, LR}`;
   the FSP port lost that bracket when splitting one BLX into three. Result: a
   *nested* interrupt clobbers the outer ISR's return address → Data Abort minutes
   to hours later, with unusable DFAR/LR. Fix: `lr` added to the entry/exit frame.
   Validated: 5 h, 41 M interrupts, 681 nested events, zero faults (unfixed
   baseline: one crash per 8–60 nested events).
   Also restores upstream's `ulPortInterruptNesting` (context switch only at
   nesting level 0).
2. **`bsp_irq.c` — `g_vector_table[gic_intid]` had no bounds check** (table has
   480 entries, GIC INTIDs go to 1022). Out-of-range IDs now log instead of
   calling a garbage pointer.

If you rebuild the CA55-side libmetal/libopen-amp: use `dmb sy` (or `osh`) fences,
not `ish` — the CR8 is outside the CA55 inner-shareable domain and Normal
Non-cacheable accesses are architecturally outer-shareable. Details in
`docs/04-communication.md`.

## Boards

- **RDK**: full sensor stack in production use (GPS UBX on RSCI5, IST8310 compass
  on RSCI7 I2C, 5 Hz snapshots over rpmsg), 44 h continuous soak on record.
- **EVK**: same kernel + firmware, brought up with `docs/01`; DTB in `dts/` has the
  RSCI5 handover applied. rpmsg echo p99.9 = 0.2 ms.

## License / provenance

FSP-derived files retain their original Renesas license headers. Everything else
in this repository: MIT. Issues found in vendor code have been reported upstream
(renesas-rdk/rzv2h_drone_px4#2).

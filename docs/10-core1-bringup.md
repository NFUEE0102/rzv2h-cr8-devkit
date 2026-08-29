# 10 — CR8 core1 bring-up (both R8 cores running side by side)

First time ever on this board (2026-08-29): **both Cortex-R8 cores running
concurrently**, each with its own rpmsg channels, while the CM33 region stays
untouched. Verified: core1 echo 2000/2000 (avg 0.18 ms) *while* core0 served
the PWM web demo; both black boxes ticking; `0x43000000` (CM33's region)
bit-stable throughout.

## core1's layout (mirrors docs/09's per-core scheme)

| Item | core0 | core1 |
|---|---|---|
| remoteproc | remoteproc1 | **remoteproc2** |
| TCM (AXI view) | 0x12040000 | **0x12080000** |
| SRAM window | 0x08180000 | **0x08200000** (see mine #3) |
| DDR window | 0x40800000 | **0x41800000** |
| resource table | 0x42F02000 | **0x42F04000** (`rsctbl-cr8-1`) |
| vrings ch0/ch1 | 0x43800000/0x43900000 | **0x44000000/0x44100000** |
| shm ch0/ch1 | 0x43A00000/0x43D00000 | **0x44200000/0x44500000** |
| black box | 0x439F0000 | **0x441F0000** (`bb.py` auto-detects) |
| MHU msg/rsp ch | 3 / 6 (doorbell clear 0x10480068) | **4 / 7** (clear **0x10480088**) |
| MHU rx IRQ | 314 | **315** |
| tick timer | GTM ch4 | **GTM ch5** (see mine #1) |

Firmware: `core1` branch of the project (Core=CR8_1 in `configuration.xml`
regenerates the BSP; `custom_rzv2h_cr8_1.ld`; the demo skips every shared
peripheral — no LED, no GPT — pure echo + black box).
Client: `examples/linux-client` with `MBX_ID = UIO_RECEIVER3` and the core1
addresses/names (built as `r8_bench_c1`; the transport `.c` files need zero
changes — the CR8_1 mailbox entry always existed).
Start: `tools/start-cr8-core1.sh <fw.elf>`.

## Three mines found the hard way

1. **Both cores on one tick timer = the second core kills the first.**
   The FSP demo config uses GTM ch4; a core1 build inheriting it takes over
   core0's tick source and re-routes its interrupt — core0's scheduler stops
   dead within seconds (uptime frozen, no fault logged, rpmsg silent).
   **Every shared peripheral must be per-core**: tick GTM, LED, GPTs, and the
   GIC lines they imply. core1 uses GTM ch5. (Linux probes all OSTMs but uses
   none of their interrupts — the ARM arch timer serves Linux — so ch4/ch5
   are safe to take.)
2. **The `.wait_data_other` segment loads into the *other* core's SRAM.**
   FSP emits two 4-byte init-arbitration words, one per core; the "other
   core" copy sits in the other core's SRAM window, so remoteproc refuses the
   ELF (`bad phdr da 0x81bfffc -22`) — and if it *did* load, it would smash
   the other core's live arbitration word. `tools/null_phdr_at.py <elf>
   0x081BFFFC` nulls it for core1 builds (0x081FFFFC for core0 builds; not
   needed there today only because core0's window happens to cover both).
3. **The kernel's SRAM split is not the FSP template's split.** FSP convention
   halves 0x08180000+256K/core; the device tree gives each core a full window
   — core1's is `0x08200000/0x80000`. The DT is what remoteproc enforces:
   link to the DT window or the ELF is rejected.

## Known-good limits

- Commands other than echo on core1's demo: PWMC returns an error status (its
  GPTs are never opened — they belong to core0); JITQ returns zeros.
- `bb.py` picks the first live black box (core0 first); use
  `--addr 0x441F0000` to read core1 explicitly.
- CM33 remains the last core: its region is now genuinely free, but its
  earlier bring-up attempts hard-froze pre-UART (archived); the DT's
  `renesas,rz-bootaddrs = <0x08003000 0x18003000>` differs from the address
  those attempts used — worth one retry before reaching for J-Link.

## Addendum (2026-08-30): mines revisited

- **Mine #5 (see docs/12)**: never restart one R8 core while the other runs —
  no per-core reset exists; a halted core resumes from its old PC inside the
  freshly loaded image. `tools/restart-r8-pair.sh` is the lawful path and the
  start scripts now enforce it.
- **Mine #1 correction**: the original "GTM ch5" build never actually
  regenerated (e2studio cache) — the echo demo had been running a *shared*
  GTM ch4 the whole time, which happens to co-run once both cores are up
  (the first-start freeze was the takeover during core1's initial GTM open).
  The current core1 build genuinely uses ch5; verify via
  `rzv_gen/vector_data.c` (`GTM5 INT`), never via the XML alone.

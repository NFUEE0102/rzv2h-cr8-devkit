# 09 — Per-core carveout migration (CR8_0 moved out of the CM33 region)

Until 2026-08-29 every firmware in this kit squatted in the **shared rpmsg
region that the device tree assigns to the CM33** (resource table at
`0x42F00000`, vrings at `0x43000000`, the region the factory design shares
between the CM33 and U-Boot-loaded firmware). It worked, but it meant the CM33
and CR8 core1 could never run rpmsg alongside core0.

As of the per-core migration, core0 lives where the device tree always
intended:

| Item | Shared layout (legacy) | Per-core layout (current) |
|---|---|---|
| resource table | `0x42F00000` (= CM33's) | **`0x42F02000`** (`rsctbl-cr8-0`) |
| ch0 vrings / shm | `0x43000000/0x43050000` / `0x43200000` | **`0x43800000/0x43850000` / `0x43A00000`** |
| ch1 vrings / shm | `0x43100000/0x43150000` / `0x43500000` | **`0x43900000/0x43950000` / `0x43D00000`** |
| linker vring window | 15 MB (`0x43000000+0xF00000`) | **8 MB** (the exact cr8_0 carveout) |
| black box | `0x431F0000` | **`0x439F0000`** (tail of the ctl1 window) |
| MHU shmem | `0x42F01000` | unchanged (single node, shared by design) |
| UIO node names | `43000000.vring-ctl0` … | `43800000.vring-ctl0-c0` … |

CR8 core1's mirror set (`0x42F04000`, `0x44000000/0x44100000/0x44200000/
0x44500000`) is prepared in the DT the same way; core1 bring-up is the next
step.

## What had to change (the complete list — three of these were discovered
the hard way, they were missing from the first migration plan)

**Device tree** (runtime `fdtput` on the on-board DTB; `tools/percore-board.sh`
does all of it):
- 8 new root-level UIO nodes (`vring_uio`/`shm_uio`) for the per-core windows
  of both cores. The `rsctbl-cr8-0/1` nodes already existed.
- `cr8-uio-bind.sh`: 10 new patterns. Old nodes keep their uio numbers
  (uio0-9); the new ones appear as uio10-19.

**Firmware** (all on the `percore` branch of the project):
1. linker script: rsctbl/vring origins + 8 MB length
2. `OpenAMP_RPMsg_cfg.h`: six addresses + four UIO node names
3. **`rsc_table.h`: `CFG_RSCTBL_MEM_PA` → `0x42F02000`** — missed by the
   original plan. The R8 polls the resource table *at this address* at runtime;
   with the old value it polls the old address and the handshake never
   completes (client says `communication abort`, black box shows `mhu_isr=0`).
4. **`mpu_region_table.c`: region 8 grown 8 KB → 32 KB** — also missed.
   The old 8 KB region covered only `0x42F00000-0x42F01FFF`; the new resource
   table at `0x42F02000` fell outside every MPU region and faulted. (This was
   the mystery "bus block at 0x42F02xxx" — it was our own MPU table, not a
   firewall.)
5. black box moved to `0x439F0000` (the old home is inside the CM33 region)

**Linux side**:
- client (`examples/linux-client`): same six addresses + four names in
  `OpenAMP_RPMsg_cfg.h`, plus `rsc_table.h` dev-name/PA → `rsctbl-cr8-0`.
  Note the Makefile has no header dependencies — `make clean && make` after
  header edits, or you will run the old binary and chase ghosts.
- **`tools/start-cr8.sh`: clears the virtio status bytes of *both* resource
  tables** (`0x42F00044/B4` and `0x42F02044/20B4`) and accepts the boot-health
  check from either black-box address — so it serves shared-layout and
  per-core firmware alike. Also missed by the original plan.
- `bb.py`: auto-detects the black box at `0x439F0000` (per-core) or
  `0x431F0000` (legacy), `--addr` to force.

## Compatibility

- **Per-core firmware needs the per-core client and vice versa.** The rebuilt
  `r8_bench` only talks to per-core firmware; the previous binary is kept on
  the board as `r8_bench.shared-layout` for the legacy ELFs.
- `cr8_pwm_demo.elf` and `cr8_sensors.elf` are per-core builds now.
  `cr8_demo_patched.elf` (the minimal echo demo) is **legacy shared-layout** —
  the walkthrough uses `cr8_pwm_demo.elf` for the echo test instead (every
  firmware in this kit echoes non-command packets).
- Verified after migration: echo 1000/1000, PWM web demo end to end, black box
  live at the new address, and the CM33 region (`0x43000000`) stays bit-for-bit
  untouched while core0 runs traffic — that region is now genuinely free for
  the CM33.

## Rollback

Old DTB kept as `/boot/*.dtb.pre-percore`, old start script as
`start-cr8.sh.pre-percore`, old client binary as `r8_bench.shared-layout`,
legacy elf `cr8_demo_patched.elf` untouched. Restore those four and you are
back on the shared layout.

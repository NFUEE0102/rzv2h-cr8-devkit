# 10 — Running the second R8 core

Both Cortex-R8 cores can run concurrently, each with its own rpmsg link.
This chapter covers starting/stopping them safely and building a core1
firmware. Memory assignments: docs/09.

## Start / stop rules

```sh
sudo ~/devkit/tools/start-cr8.sh cr8_pwm_demo.elf          # core0
sudo ~/devkit/tools/start-cr8-core1.sh cr8_core1_demo.elf  # core1
```

- Starting a **cold** core while the other runs is fine.
- Stopping a core is fine — the sibling is unaffected.
- **Restarting a core that has already run, while the sibling is running, is
  not possible**: the CR8 cluster has no per-core reset, so such a core is
  only halted and would resume from a stale program counter inside the newly
  loaded image. The start scripts detect and refuse this. To restart or swap
  firmware on either R8 core, use:

```sh
sudo ~/devkit/tools/restart-r8-pair.sh [core0.elf] [core1.elf]
```

It takes both cores down (a full cluster reset), then starts core0 and core1
in order. Budget ~10 seconds of downtime for both cores.

## Building a firmware for core1

Port checklist, applied to a core0-based FSP project (all verified spots —
skip one and the core will not come up):

| Item | core1 value |
|---|---|
| `configuration.xml` → `<option key="Core">` | `CR8_1` (regenerates BSP + memory regions) |
| MHU module channel | 4 (rx IRQ becomes 315) |
| tick timer (GTM module channel) | **5** — never share a tick channel between cores |
| linker script | core1 variant: TCM 0x12080000, **SRAM 0x08200000** (the device tree's window — not the FSP template's 0x081C0000), DDR +0x01800000, rsctbl +0x02F04000, vrings +0x04000000 |
| `src/rsc_table.h` | `42f04000.rsctbl-cr8-1` / `0x42F04000` |
| `src/OpenAMP_RPMsg_cfg.h` | the core1 column of docs/09 (six addresses + four `-c1` UIO names) |
| MPU table | core1 branch of `mpu_region_table.c`: vring region base 0x44000000 |
| black box | `0x441F0000` |
| shared peripherals | do not touch core0's: its GPTs, the LED, GTM ch4 |

After every build:

1. **Verify regeneration**: `grep GTM rzv_gen/vector_data.c` must show the
   channel you configured (`GTM5 INT` for core1). The XML alone is not proof.
2. **Neutralize the cross-core init word**: the toolchain emits a 4-byte
   `.wait_data_other` segment inside the *other* core's SRAM; remoteproc
   rejects the ELF over it. Run:

```sh
python3 tools/null_phdr_at.py <firmware.elf> 0x081BFFFC   # core1 builds
```

3. `tools/patch-elf-phdr.py` as usual (docs/03).

Linux client: `examples/linux-client` with `MBX_ID = UIO_RECEIVER3` and the
core1 addresses/names — build it as a separate binary (e.g. `r8_bench_c1`)
so the core0 client stays usable.

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| black box: `boot#` does not increment, startup probe stays at its sentinel, garbage-address fault with `sp_svc=0` | the core was restarted alone after having run (see rules) | `restart-r8-pair.sh` |
| client prints `communication abort`, black box shows `mhu_isr=0` | firmware's `rsc_table.h` PA does not match the client's — the R8 polls the wrong resource table | align both to docs/09 and rebuild |
| `bad phdr da 0x81bfffc … -22` on start | `.wait_data_other` segment not neutralized | `null_phdr_at.py` step above |
| core1 runs but its sibling's scheduler freezes | both firmwares configured the same GTM tick channel | give each core its own channel |
| both R8 rpmsg links dead after running a client by hand | second client on an owned channel corrupted the vrings | stop the web daemons first; recover with `restart-r8-pair.sh` |

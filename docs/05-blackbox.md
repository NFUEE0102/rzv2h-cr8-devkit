# 05 — The black box (acceptance & diagnostics)

The CR8 has no console in this configuration, and when things go wrong rpmsg is
usually the thing that's broken — so it can't carry the diagnosis. The black box
is a fixed struct the firmware writes into DDR that Linux can always read:

- **Address `0x431F0000`** — tail of the `vring-ctl1` window: already UIO-mapped
  on the Linux side, inside the R8's Normal-NC MPU region (writes are immediately
  visible), and far beyond the ~27 KB the vrings actually use.
- Survives R8 crashes and restarts; `boot_count` persists across warm restarts so
  you can tell a fresh run from leftovers.

## Integrating into firmware

Add `blackbox/r8_blackbox.c/.h` to the project and call as early as possible:

```c
/* in R_BSP_WarmStart(BSP_WARM_START_POST_C): */
r8_blackbox_init();
R8_BB_STAGE(1);
```

Everything else is optional and weak-linked: FreeRTOS hooks
(`configCHECK_FOR_STACK_OVERFLOW=2`, `configUSE_MALLOC_FAILED_HOOK=1`), C
overrides of the `.weak` CPU exception handlers (they latch DFSR/DFAR/IFSR/IFAR
— without these, any abort is an instant, silent full-core freeze), stage
markers, and per-interrupt probes. Fault paths call a weak
`pwm_emergency_stop()` **before** disabling interrupts — PWM hardware keeps
driving its last duty cycle when the core stops; disarm outputs first.

## Reading

```sh
sudo python3 blackbox/bb.py        # full dump
sudo python3 blackbox/bb.py -q     # verdict + stack watermarks only
```

`bb.py` uses `busybox devmem` on purpose: Python's `mmap('/dev/mem')` gets a
*cacheable* mapping of this region and the attribute mismatch kills the process
(taking the SSH session with it).

Key fields (offsets are stable — new fields are only ever appended):

| Offset | Field | Meaning / healthy value |
|---|---|---|
| 0x00 | magic `R8BB` | anything else ⇒ firmware never ran `r8_blackbox_init` |
| 0x08 | boot_count | increments per firmware start |
| 0x40 | boot_stage | where boot got to; app stage when healthy |
| 0x44 | fault_type | **0.** 1=stack ovf 2=malloc 3=data abort 4=prefetch 5=undef |
| 0x48/0x4C/0x50 | DFAR / DFSR / LR | latched at fault (async faults: treat DFAR/LR as unreliable) |
| 0x10–0x38 | heartbeats & IPC counters | advancing; `sem_give==sem_take`; `notif_err==send_err==0` |
| 0x64–0x6C | SVC/IRQ/SYS stack high-water | painted at boot, scanned ~1 Hz |
| 0x154 | tick_count | **the** liveness signal — written by the tick ISR itself, keeps moving even if every task is stuck |
| 0xC8 | nest_max | max interrupt nesting depth observed |
| 0xEC… | ring buffers | last 8 interrupts: ID, sp, depth (+ per-frame checks) |

## Acceptance checklist

1. `magic` ok, `fault_type == 0`, boot stage at application level.
2. `tick_count` strictly increasing across two reads a few seconds apart.
3. Heartbeats advancing; semaphore give/take balanced; zero error counters.
4. Echo benchmark: N/N ok, zero errors, p99.9 < 0.5 ms.
5. Stack watermarks sane (our stack peaks at <5% of SVC after days of soak).
6. For long soaks: `boot_count` unchanged (no watchdog rescues) over the window.

## Diagnostic lessons baked into the design

- Counters copied into the box by a task (`pump`) go stale the moment that task
  hangs — every fault-path value is therefore re-read **directly from SRAM by
  the abort handler**. Trust those, not the periodic copies.
- On imprecise (async) external aborts, DFAR and LR are decoys. The per-interrupt
  ring buffer and independent tick counter are what actually localize the fault —
  they are how both root causes shipped in `fsp-patches/` were found.

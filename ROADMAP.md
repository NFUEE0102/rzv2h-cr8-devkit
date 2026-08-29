# Roadmap

## 2.0 — flight-grade core management

Driver: the mine #5 restriction (docs/12) is incompatible with **in-flight
redundancy** — restarting one R8 core must not interrupt the other. Planned
work, in order:

1. **Parking-stub single-core restart (cooperative case).**
   A tiny stub at a fixed ITCM address, byte-identical across every firmware
   (shared `.S`): on a park request (black-box flag + MHU kick) the firmware
   masks IRQs and spins in the stub; the host verifies `stage=PARKED`, halts
   the core, loads the new image (the stub region is overwritten with the
   same bytes), sets the GO flag, un-halts — the stub sees GO and branches to
   the reset vector of the *new* image. Clean single-core swap, sibling
   untouched, no kernel change. Start scripts grow `park`-aware paths;
   `restart-r8-pair.sh` remains the fallback.
2. **Crashed-core recovery (the hard half).**
   A crashed core cannot reach the stub, and un-halting it resumes a garbage
   PC — the parking scheme only covers cooperative restarts. Open questions
   to resolve first:
   - manual deep-dive: does SYSC/CPG hide any per-core warm-reset control
     the current driver doesn't use? (R01UH* manual archaeology)
   - if not: fault-hook hardening — on any fault the firmware itself jumps
     to the parking stub (fault handler = auto-park), making most "crashes"
     recoverable as cooperative parks; only a hard-hang (no fault taken)
     would still need the pair restart
   - flight architecture note: treat pair-restart as the last resort and put
     the fail-safe authority on a core that never restarts in flight
     (CM33 or A55 supervisor + POEG hardware failsafe).
3. **rz_rproc stop-path memset fix (kernel).** The DT window split (docs/11)
   already scopes the CM33 wipe correctly; the driver-side fix (skip
   `reg`-window carveouts in the stop memset, ~5 lines) is the upstreamable
   correction and also lifts the "CM33's own black box wiped on stop" caveat.
4. **POEG hardware failsafe** before any real ESC/servo is attached.
5. **FSP housekeeping**: the AAPCS sp≡4(mod 8) one-liner (separate commit +
   its own soak), diagnostic-instrumentation flag-gating, and the formal FSP
   report (pending Renesas contact).

## 1.x

- **v1.1.0 image — SHIPPED 2026-08-30**: three-core firmware set + three
  clients + guarded tools + docs 09-12 + three-core black box, on the
  mine-4-fixed DTB. 26-point verified capture (20 file md5s against the live
  board + 6 absence checks + re-captured p1 with the new DTB).

### Next update (1.x maintenance candidates)

- **Warm-reboot hang diagnosis**: software `reboot` hangs before BL2 ever
  prints (serial stays silent) — the PSCI SYSTEM_RESET path in the custom
  TF-A (`v2h_1.0.4-dirty`) does not actually reset the SoC. A serial recorder
  is parked on the ground station; one deliberate `reboot` with it attached
  captures the full fingerprint (kernel "Restarting system" then silence).
  Costs one power-cycle. Fix lands in TF-A, not in this kit's kernel.
- Sensors web demo (`serve.py`/`start-demo.sh`) refresh for the per-core
  world, or formal retirement note in docs.

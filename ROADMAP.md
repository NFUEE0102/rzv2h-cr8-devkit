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

- **v1.1.0 image**: three-core firmware set + three clients + guarded tools +
  docs 09-12 + three-core black box, on the mine-4-fixed DTB. Material is
  complete on the board; capture/verify/release pipeline is proven.

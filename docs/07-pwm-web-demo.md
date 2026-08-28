# 07 — PWM web-control demo (write your own firmware)

Every earlier chapter runs firmware that ships with this kit. This chapter walks
the full **write-your-own-firmware** path: the sensor firmware was stripped down
to a **pure communication-test firmware** (no GPS, no compass) that drives
**hardware PWM from a GPT timer**, while the CA55 serves a web page with knobs
that set the **frequency** and **duty cycle** live:

```
browser (knobs)
   │ HTTP POST /api/pwm
   ▼
serve_pwm.py (CA55, :8080) ──one line per cmd, stdin──▶ r8_bench pwmd (resident,
   ▲                                                     owns the rpmsg channel)
   │◀──── /run/r8pwm.json (atomic rewrite per cmd) ◀──── │ PWMC (r8_pwm_proto.h, ABI v4)
                                                         ▼
                                          Cortex-R8: R_GPT_Stop→PeriodSet→DutySet→Start
                                                         ▼
                                          GTIOC6A = P76, GTIOC6B = P77 (hardware out)
```

The "R8 report" table on the page shows the **register values the R8 actually
applied** (period/duty counts; PCLK reported by the R8, not assumed by the
A55) — that is the closed loop. UI text is zh-TW, same as the other on-board
tools (`bb.py`).

## Run it

Image v1.0.2 and later ships everything (`/lib/firmware/cr8_pwm_demo.elf` +
`~/r8web/`):

```sh
sudo ~/r8web/start-pwm-demo.sh     # swaps firmware if needed + starts the web UI
# then browse to http://<board-ip>:8080/
```

Manual install (older image): copy `examples/pwm-web-demo/` into `~/r8web/`,
`examples/firmware/cr8_pwm_demo.elf` into `/lib/firmware/`, rebuild
`examples/linux-client/` (`make` — the `pwmd` mode lives in `main.c`) and place
`r8_bench` in `~/r8_bench/`.

## What changed vs. the sensors firmware

Same project, same FreeRTOS patches, same black box and rpmsg stack. Six edits:

| Change | Why |
|---|---|
| `sensors_start()` and the sensor task removed | pure communication demo, no GPS/compass |
| SNSQ branch removed from rpmsg cb1 | sensor-query packets now take the echo path |
| P76/P77 rebound from RSCI7 (I2C) to GTIOC6A/B | compass gone, pins returned to GPT6 — also fixes the old "GPT6 replies OK but no pin output" defect |
| all channels boot at duty=0 (quiet) | the old boot pattern drove every pin at power-up; dangerous with a servo/ESC attached |
| `r8_blackbox_pump()` moved into the PWM heartbeat loop | the sensor task used to call it; without the move most black-box fields freeze |
| jitter-probe timebase GPT6 → GPT0 | GPT6 is now the demo's main output; changing its period would wreck the statistics |

## Pins

**On the EVK, stick to GPT6 (P76/P77)** — verified free, and physically on the
**CN3 Pmod header** (full header maps: docs/08-evk-pmod-pinout.md). The
firmware accepts commands for the other GPTs too, but their pins have traps:

| GPT | Out | Pins | Where / EVK caveat |
|---|---|---|---|
| 6 | A / B | **P76 / P77** | ✅ **CN3 pin 9 / pin 10** (GND on CN3 pin 5/11) — the demo channel |
| 0 | A | P70 | CN6 pin 7 — jitter-probe timebase; changing it wrecks the stats |
| 5 | A / B | P84 / P85 | **= Linux's ttySC1 (SCI4 UART). Don't.** Not on a Pmod anyway |
| 8 | A | P50 | CN3 pin 2 — factory-bound to Linux SPI (docs/08) |
| 7 | B | PA7 | CN2 pin 1 (shares the header with RSPI2 nets); A (PA6) unreachable |
| 4 / 9 | B; A/B | P95; P96/97 | not on any Pmod header |

## A and B share the period (hardware fact)

Both outputs of one GPT share a single counter and period register (GTPR):
**frequency is per GPT, not per output**. Changing the frequency silently
changes the other output's effective duty (its compare value is in counts), so
the web page re-sends both channels' duty whenever the frequency knob moves.

## The `pwmd` resident mode (new in `examples/linux-client/main.c`)

The rpmsg channel must be held by a **single resident client** (repeated
open/close cycles park the MHU mid-handshake — see docs/04). The web backend
therefore keeps one daemon:

```
r8_bench pwmd [json-path]        # default /run/r8pwm.json
  stdin, one line per command:  <gpt> <a|b> <freq_hz> <duty_permille>
  each command: send PWMC, await reply, write the R8 report as one-line JSON
                (tmp+rename, atomic)
  stdin EOF or SIGINT → full release_channel teardown
```

Try it without the web layer — **stop the web service first** (Ctrl-C, or
`sudo kill -INT <serve_pwm.py pid>` and wait a few seconds for its daemon to
tear down). The channel takes exactly one client: a second `pwmd` started while
the web backend holds it tramples the shared vring and knocks the R8 back to
its reconnect state. Then, in `~/r8_bench`:

```sh
printf "6 a 5000 750\n" | sudo ./r8_bench pwmd && cat /run/r8pwm.json
```

(Restore the demo afterwards with `sudo ~/r8web/start-pwm-demo.sh`.)

Or against the running backend:

```sh
curl -X POST http://<board-ip>:8080/api/pwm \
     -H "Content-Type: application/json" \
     -d '{"gpt":6,"ch":"a","freq_hz":50,"duty_permille":75}'   # servo neutral, 1.5 ms
```

## Shutdown rules (important)

- Stop the web service with `Ctrl-C` (foreground) or `kill -INT <serve_pwm.py
  pid>` — the server closes the daemon's stdin and the daemon tears down
  cleanly on EOF.
- **Never `kill -9` pwmd / r8_bench**: the MHU parks mid-handshake, nothing can
  reconnect, and only an R8 restart (`start-cr8.sh` does the MHU cleanup)
  recovers it.
- Restart with `restart-pwm-web.sh` (idempotent; `start-pwm-demo.sh` uses it).
- Background-start trap: `sh -c "... &"` leaves SIGINT ignored in the child.
  `serve_pwm.py` explicitly reinstalls handlers, so SIGINT/SIGTERM both work.

## Known limits

- Web knob range 1 Hz–100 kHz (the number box accepts up to 1 MHz). PCLK is
  200 MHz with integer division, so the frequency error is the rounding to
  whole counts (e.g. 731 Hz applies as 731.0022 Hz).
- Duty resolution is 1‰ (the protocol speaks permille, so a servo's 7.5% is
  exact).
- Think failure modes before attaching real hardware: if the R8 dies, the GPT
  **keeps driving the last duty cycle** (it's hardware). The fault hook calls
  `pwm_emergency_stop()`, but there is no POEG-level hardware failsafe yet —
  do not connect propulsion without an independent kill switch.

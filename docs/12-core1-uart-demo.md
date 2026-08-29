# 12 — core1 UART demo (two R8 cores, two jobs, one web page)

Division of labour, all over rpmsg, all on one page (`http://<board-ip>:8080/`):

- **core0** drives hardware **PWM** (docs/07 — knobs for frequency/duty,
  output on Pmod CN3 pin 9/10)
- **core1** drives a **UART terminal**: type text in the browser → rpmsg →
  core1 → **RSCI5 TX on P72 (Pmod CN6 pin 8)**, 115200 8N1; whatever arrives
  on **RX P73 (CN3 pin 8)** streams back to the page.
  Jumper P72→P73 for instant loopback echo; or wire a real device / probe TX
  with a scope.

```
browser ── /api/pwm ──▶ serve_pwm.py ── stdin ─▶ r8_bench pwmd  ── rpmsg ─▶ core0 ─▶ GPT6
        ── /api/uart/send, /api/uart/status ──▶ r8_bench_c1 uartd ─ rpmsg ─▶ core1 ─▶ RSCI5
```

## Protocol (`r8_uart_proto.h`, ABI v1)

Same endpoint as the PWM protocol, dispatched by magic: `uart_cmd_t`
(op SEND = data[len]→TX / op QUERY = drain the 1KB RX ring) →
`uart_rsp_t` (status, tx/rx/dropped totals, up to 384B of RX). The `uartd`
resident mode sends a QUERY every ~200 ms and mirrors state into
`/run/r8uart.json`; `send <text>` lines on stdin become TX.

Try it without the web layer (stop the web service first — single-client rule):

```sh
printf "send hello\n" | sudo ./r8_bench_c1 uartd && cat /run/r8uart.json
```

## Firmware

`core1-uart` branch = the core1 echo demo plus RSCI5 (P72/P73 pins were freed
when the sensors firmware retired; the DT already hands RSCI5 to the R8).
UART interrupts at IPL 14 — safely nested thanks to the docs/09-era lr fix.
The startup probe (black box `+0x40` = `0xE1/0xE2` markers before `bb_init`)
ships in this build — see mine #5 below, it is the diagnostic for it.

## ☠ Mine #5 — never restart one R8 core while the other is running

The CR8 cluster has **no per-core reset**. Stopping one core while its sibling
runs only *halts* it; the next start loads the new image and un-halts — the
core resumes from its **old PC inside the new image** and runs off the rails.
Black-box fingerprint: `boot#` does not increment, the startup probe stays at
its sentinel, a garbage-address prefetch/undefined fault with `sp_svc=0`.

- The start scripts now refuse this combination (a once-run core, sibling
  running); `tools/restart-r8-pair.sh` is the lawful path: both cores down
  (full cluster reset), then core0, then core1.
- A cold core starting while the sibling runs is fine (it sits at the reset
  vector) — that is how the pair script's second step works.
- Historical note: this also re-dates the docs/10 mine #1 story — the original
  "GTM ch5 fix" build never actually regenerated (e2studio cache; the echo
  build kept running a *shared* GTM ch4, which happens to co-run), and the
  first freeze was the ch4 takeover during core1's very first GTM open. The
  UART build regenerates for real: core1 now genuinely ticks on **GTM ch5**.
  Trust `rzv_gen/vector_data.c` (`GTM5 INT`), not the XML, when verifying.

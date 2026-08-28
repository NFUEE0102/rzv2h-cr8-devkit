# PWM web-control demo

Web knobs → HTTP → rpmsg → Cortex-R8 → hardware PWM on P76/P77.
Full guide: [docs/07-pwm-web-demo.md](../../docs/07-pwm-web-demo.md).

| File | Install to | Purpose |
|---|---|---|
| `serve_pwm.py` | `~/r8web/` | HTTP :8080, owns the pwmd daemon |
| `pwm.html` | `~/r8web/` | knob UI (frequency + duty A/B, zh-TW) |
| `start-pwm-demo.sh` | `~/r8web/` | one shot: swap firmware + start web |
| `restart-pwm-web.sh` | `~/r8web/` | idempotent restart (never SIGKILLs the daemon) |
| firmware | `/lib/firmware/cr8_pwm_demo.elf` | from `../firmware/cr8_pwm_demo.elf` |
| client | `~/r8_bench/` | build `../linux-client/` (`make`; `pwmd` mode included) |

```sh
sudo ~/r8web/start-pwm-demo.sh     # then browse http://<board-ip>:8080/
```

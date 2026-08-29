# 00 — Getting started (first time with this board)

This walkthrough assumes **nothing**: you just received an RZ/V2H EVK ver1, you
have never touched remoteproc or FSP, and you want to see the Cortex-R8 run and
talk to Linux — then build your own firmware for it.

## What you need

- RZ/V2H **EVK ver1** board + power supply
- micro SD card, **16 GB or larger**, and a card reader
- Ethernet cable to your LAN (the board gets its address by DHCP)
- a PC with [balenaEtcher](https://etcher.balena.io/) (or `dd` on Linux/macOS)

## Step 1 — Flash the SD card

Download `evk-devkit-16gb-v1.0.2.img.xz` (or newer) from the
[Releases page](https://github.com/NFUEE0102/rzv2h-cr8-devkit/releases).
Etcher flashes the `.img.xz` directly — select the file, select the card, Flash.
(Or: `xz -dc evk-devkit-16gb-*.img.xz | sudo dd of=/dev/sdX bs=4M conv=fsync`.)

Insert the card, connect Ethernet, power on. Give it ~60 seconds.

## Step 2 — Log in

Find the board's IP (check your router's DHCP table, or
`ping rzv2h-evk`/scan the subnet), then:

```sh
ssh ubuntu@<board-ip>          # password: ubuntu
```

## Step 3 — Know what's already on the board

The image ships with everything pre-installed. You do **not** need to build
anything to get your first result:

| Where | What |
|---|---|
| `/lib/firmware/cr8_pwm_demo.elf` | **Start with this one** — echoes every non-command rpmsg message *and* carries the PWM demo + black box. (`cr8_demo_patched.elf` also exists but is legacy shared-layout — docs/09.) |
| `/lib/firmware/cr8_sensors.elf` | R8 sensor firmware (GPS + compass + black box). Runs fine with no sensors attached — it just reports 0 samples. |
| `~/r8_bench/` | Linux-side rpmsg client, **already compiled** (`r8_bench`), sources + Makefile included |
| `~/r8web/bb.py` | black-box reader (R8 health, from Linux, no console needed) |
| `/dev/uio0..9` | shared-memory + doorbell windows (appear automatically at boot) |

Image **v1.0.2 and later also ships this repository at `~/devkit`** — nothing
to fetch. Update it anytime with `git -C ~/devkit pull`. Only on the older
v1.0.0/v1.0.1 images do you fetch it once:

```sh
git clone https://github.com/NFUEE0102/rzv2h-cr8-devkit.git ~/devkit   # v1.0.0/v1.0.1 only
```

> ⚠️ Only if you flashed the older **v1.0.0** image: its on-board `~/start-r8-sensors.sh`
> checks `/dev/ttySC1`, which on the EVK is a *different* serial port, so it
> refuses to run even though everything is fine — use `~/devkit/tools/start-cr8.sh`
> there. **v1.0.1 and later images have this fixed** (the check reads the DT
> node status directly).

## Step 4 — Start the R8 (first firmware)

```sh
sudo ~/devkit/tools/start-cr8.sh cr8_pwm_demo.elf
```

> `cr8_demo_patched.elf` (the minimal echo-only demo) is **legacy**: it uses
> the old shared memory layout and only talks to the old client
> (`r8_bench.shared-layout`). Every current firmware echoes non-command
> packets, so `cr8_pwm_demo.elf` serves the echo test — see docs/09.

What this does, so it isn't magic: it stops the core if needed, clears a
possibly-stale doorbell and two handshake bytes, tells remoteproc which file in
`/lib/firmware` to load (`.../remoteproc1/firmware`), and starts the core
(`.../remoteproc1/state`). `remoteproc1` **is** the Cortex-R8; the kernel loads
the ELF into the R8's memory and releases it from reset.

You should see `R8 running (cr8_pwm_demo.elf)`. Kernel messages about
"no resource table" during start are normal for this transport.

## Step 5 — Talk to it

```sh
cd ~/r8_bench
sudo R8_BENCH_NOTS=1 ./r8_bench 1000
```

> **Single-client rule**: if the PWM/UART web service is running
> (`start-pwm-demo.sh`), stop it before running `r8_bench` by hand — its
> daemons already hold the core0/core1 rpmsg channels, and a second client
> tramples the shared vrings (both sides then time out until the cores are
> pair-restarted). Test order on a fresh card: **echo tests first, web last.**


Expected: `1000/1000 ok, 0 errors`, average round-trip ≈ 0.15 ms. That's Linux
sending 1000 messages through shared memory and the R8 echoing each one back.

## Step 6 — Check the R8's health (works even when rpmsg doesn't)

```sh
sudo python3 ~/r8web/bb.py -q
```

The R8 has no serial console here. Firmware built with the black box
(`cr8_sensors.elf` has it; the echo demo does not) continuously writes its
health — heartbeats, boot stage, crash registers — to a fixed DDR address that
Linux can always read.

> Note: DDR is not cleared between firmwares. While the echo demo runs, `bb.py`
> shows the **previous** black-box firmware's leftover data, not live state. To
> tell live from leftover: read twice a few seconds apart — if `uptime` doesn't
> advance, you're looking at a stale image. `死因/fault: none` + advancing tick = alive. Full field
guide: `docs/05-blackbox.md`.

Try it with the sensor firmware:

```sh
sudo ~/devkit/tools/start-cr8.sh cr8_sensors.elf
sudo python3 ~/r8web/bb.py -q
```

(No GPS/compass wired up? Fine — heartbeats advance, sample counters stay 0.)

## Step 7 — Build and deploy your own firmware

1. **Project**: start from Renesas'
   [`rzv2h_rdk_cr8_core0_rpmsg_linux_rtos_demo`](https://github.com/renesas-rdk/rzv2h_rdk_cr8_core0_rpmsg_linux_rtos_demo)
   in e2 studio with FSP 3.1.0 (the companion `..._cm33_...` project must sit in
   the same workspace — the CR8 project references its `.sbd`).
2. **Apply the fixes**: copy the three files from this repo's `fsp-patches/`
   over `rzv/fsp/src/...` in your project. Without the `portASM.asm` fix, any
   firmware that uses two interrupt priorities will crash randomly under nested
   interrupts (that bug cost us a week — details in the file headers).
3. **Build** (hammer icon). Your firmware is
   **`Debug/<project-name>.elf`** — that single file is the entire deliverable.
4. **Fix the ELF headers** (required once per build, on any machine with
   python3):

   ```sh
   python3 tools/patch-elf-phdr.py Debug/<project-name>.elf
   ```

   Skip this and the kernel refuses the file with `bad phdr ... -22` (docs/03
   explains why).
5. **Deploy to the board**:

   ```sh
   scp Debug/<project-name>.elf ubuntu@<board-ip>:/tmp/
   ssh ubuntu@<board-ip>
   sudo ~/devkit/tools/deploy-cr8.sh /tmp/<project-name>.elf
   ```

   `deploy-cr8.sh` = stop the core (and *verify* it stopped — writing the
   firmware name while it still runs silently keeps the old code), back up the
   previous ELF, install, start.
6. **Add the black box to your firmware** (strongly recommended): drop
   `blackbox/r8_blackbox.c/.h` into the project and call `r8_blackbox_init()`
   in `R_BSP_WarmStart` — see `docs/05-blackbox.md`. From then on `bb.py` can
   tell you *why* your firmware died instead of just "rpmsg stopped".

## When something doesn't work

| Symptom | Likely cause / fix |
|---|---|
| `state` says `running` but it's the old behaviour | firmware name was written while running — run `deploy-cr8.sh` again (it verifies the stop) |
| `bad phdr ... -22` on start | you skipped `patch-elf-phdr.py` (step 7.4) |
| `r8_bench`: `failed to open /dev/uio*` | run it with `sudo`; check `systemctl status cr8-uio-bind` |
| `r8_bench` hangs / no replies | stale handshake — rerun `start-cr8.sh` (it clears the doorbell + status bytes); make sure only **one** client uses the channel, and stop clients with Ctrl-C, never `kill -9` |
| `bb.py`: `magic = 0xFFFFFFFF` | no black-box firmware has run since power-on (cold DDR), or the R8 never started |
| `bb.py` shows data but `uptime` frozen | leftover from a previous black-box firmware — the current one doesn't write the box (e.g. the echo demo) |
| `bb.py` shows a fault | read `docs/05-blackbox.md` — DFSR/DFAR and the interrupt ring buffer are all captured for you |

## Next: write your own firmware

The PWM web-control demo (`docs/07-pwm-web-demo.md`) walks the full
custom-firmware path: strip the sensors, drive hardware PWM from a GPT,
and control frequency/duty from a web page over rpmsg.

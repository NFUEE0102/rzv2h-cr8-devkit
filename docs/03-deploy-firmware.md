# 03 — Replacing the R8 firmware

## Step 0 (build side): fix the ELF program headers

FSP 3.1 projects place the resource table, vrings and RTOS heap in `(NOLOAD)`
sections. The linker still emits a zero-filesize `PT_LOAD` for each, and Linux
remoteproc rejects the whole ELF:

```
remoteproc1: bad phdr da 0x42f00000 mem 0x190
remoteproc1: Failed to load program segments: -22
```

Run once after every build (it flips those segments to `PT_NULL`; self-verifying;
only touches `filesz==0 && paddr>=0x40000000`, which deliberately spares the
DTCM segment):

```sh
python3 tools/patch-elf-phdr.py path/to/firmware.elf
```

The prebuilt example in `examples/firmware/` is already patched.

## Deploying (script)

```sh
sudo tools/deploy-cr8.sh path/to/new-firmware.elf
```

which is equivalent to, and enforces the ordering of:

```sh
# 1. STOP — and verify it actually reached "offline".
#    Writing `firmware` while running fails with EBUSY but state still says
#    "running": it silently keeps executing the OLD image.
echo stop | sudo tee /sys/class/remoteproc/remoteproc1/state
# poll until: cat .../state == offline

# 2. Swap the file (the previous one is backed up automatically by the script)
sudo cp new.elf /lib/firmware/<name>.elf

# 3. Start — see tools/start-cr8.sh
sudo tools/start-cr8.sh <name>.elf
```

## The start sequence (why those three devmem writes exist)

```sh
busybox devmem 0x10480068 32 1    # clear a possibly-stale MHU ch3 doorbell
busybox devmem 0x42F00044  8 0    # virtio status, resource-table slice 0
busybox devmem 0x42F000B4  8 0    # virtio status, slice 1  ← both are needed
echo <fw>.elf > /sys/class/remoteproc/remoteproc1/firmware
echo start    > /sys/class/remoteproc/remoteproc1/state
```

Stale doorbell bits or a leftover `DRIVER_OK` from the previous session make the
new firmware and the client miss each other's handshake — the classic symptom is
a start that "works" but rpmsg never answers.

`start-cr8.sh` then reads the black box (`fault_type` at `0x431F0044`) and
retries the clean stop→clear→start cycle automatically if the firmware died at
boot — with the fixes in `fsp-patches/` applied this retry should never trigger;
it is kept as a fuse.

Kernel messages like `unsupported fw ver` / `no resource table found` during
start are **normal** for this transport: the kernel does not manage the table;
the userspace client does.

# 04 — CA55 ↔ CR8 communication

## Architecture

This transport is **userspace OpenAMP**, not kernel rpmsg. There is no
`/dev/rpmsg*`:

```
CA55 (Linux userspace)                          CR8 (FreeRTOS)
  client (examples/linux-client)                  firmware
    libopen_amp ── vrings in shared DDR ──────── open-amp
    libmetal   ──  /dev/uioN mappings             libmetal (FSP)
       │                                            │
       └── MHU ch3 doorbell (mbox-uio) ────────────┘  (IRQ both ways)
```

- Shared memory: vring descriptors at `0x43000000`/`0x43100000` (ch0/ch1),
  buffers at `0x43200000`/`0x43500000`, resource table at `0x42F00000`
  (per-channel slices at +0x00 / +0x70). Both sides map it **Normal
  Non-cacheable** — there is no cache coherency between the A55 cluster and CR8.
- Doorbell: MHU channel 3 registers at `0x10480000`, exposed to userspace via
  `mbox-uio` and its three `receiver@…` interrupt children.

## The UIO plumbing

The DT nodes carry custom compatibles (`mbox_uio` / `shm_uio` / `vring_uio`)
that no kernel driver matches. `tools/cr8-uio-bind.service` binds each one to
`uio_pdrv_genirq` at boot via sysfs `driver_override`, producing `/dev/uio0..9`.
libmetal opens them **by device name**, so numbering order never matters.

## Using the example client

```sh
cd examples/linux-client && make
sudo R8_BENCH_NOTS=1 ./r8_bench 1000     # echo benchmark, channel 1
```

Healthy numbers (both boards): ~6700 round-trips/s at 32 B, avg ≈ 0.15 ms,
p99.9 < 0.5 ms, zero errors.

Rules:

- **One resident client owns the channel.** Repeated connect/teardown can park
  the MHU mid-handshake. For apps, keep one daemon and publish (e.g. to a file);
  don't reconnect per request.
- Stop the client with **SIGINT only** (it runs `release_channel()` on the way
  out). SIGKILL leaves the channel dirty for the next user.
- `R8_BENCH_NOTS=1` skips GPT-based timestamps — without it the client SIGBUSes
  if that timer's clock isn't running.
- The client's first pass ("priming") writes/refreshes the resource table; on a
  cold board that memory contains garbage until then.

## Memory barriers — read this before rebuilding the libraries

Stock libmetal/open-amp compile `atomic_thread_fence(seq_cst)` to `dmb ish`
(inner-shareable). That is **architecturally insufficient here**:

1. CR8 is not part of the CA55 cluster's inner-shareable (coherency) domain.
2. The shared region is Normal Non-cacheable, and the ARM ARM specifies that
   such accesses are treated as **outer-shareable** regardless of the mapping's
   shareability field.

So the data is OSH-class while the fence is ISH-class. It happens to work on
this silicon because the implementation is stronger than the architectural
minimum — a latent trap, not a guarantee. Both sides in this kit use `dmb sy`.
**If you rebuild either side, keep the fences `sy` or `osh`.**

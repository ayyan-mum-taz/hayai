# hayai

The lowest-latency PlayStation 5 Remote Play client for Nintendo Switch
homebrew — plus a **controller-only mode** that turns the Switch into a
wireless gamepad for a PS5 on the TV.

*hayai* (速い — "fast") is built around one engineering goal: the shortest
possible path from a UDP datagram to a lit pixel, and from a button press to a
datagram. Every layer — network, decode, present, input, audio — is designed
for that goal, and instrumented so the claims below are measurable on the
device rather than asserted.

## Why it's fast

**Zero-copy video, end to end.** Frames are decoded by the Tegra X1's NVDEC
hardware into GPU memory and sampled *in place* by the display pass: the
decoder's own surfaces are aliased as GPU textures, and a single fragment
shader converts NV12 to RGB while drawing. The decoded image is never copied
and never touches the CPU. Typical clients pull every decoded frame back to
CPU memory and re-upload it as a texture — a two-way full-frame trip on a
mobile-class CPU, sixty times a second, that exists here as zero work.

**One thread from datagram to pixel.** The packet-receive thread decrypts,
reassembles, decodes and presents — synchronously, with no queue, no lock and
no thread handoff, pinned alone on its own CPU core at elevated priority.
Every handoff on Horizon is a scheduler wakeup with a latency tail; the hot
path has none. Clients that route frames through a UI toolkit's render loop
inherit that toolkit's scheduling; here there is no UI framework and no SDL
anywhere in the stream path.

**Present on decode, not on a schedule.** A finished frame is handed to the
display the moment it exists, through a minimal two-image swapchain in
immediate mode — the frame never waits for a render loop's next tick or for
extra swapchain depth. Against a vsync-paced UI-loop presenter this removes
roughly 8–16 ms of average added display latency (vsync remains available as
an opt-in for tear-free viewing).

**Input is sampled at the moment it is sent.** A dedicated thread samples
buttons, sticks, gyro and touch on a 2 ms cadence and hands the state to the
sender at the sampling instant, so what goes on the wire is microseconds old,
not "whatever the UI loop last polled". Polling input once per 60 Hz frame —
the common pattern — adds up to 16.7 ms of staleness before a packet ever
leaves the console. Motion data rides the same path, so gyro aiming gets the
full send rate.

**Audio is servo-controlled, not queue-managed.** The console's audio clock
and the Switch's DAC inevitably drift. Instead of letting a playback queue
grow (tens of milliseconds of creeping latency) and then flushing it (an
audible glitch), a control loop nudges a resampler by parts-per-million to
hold the buffer at a ~15 ms setpoint — indefinitely, with no resets. Opus
decoding runs on its own core, never on the network thread.

**The latency tail is engineered, not hoped for.** "Feels laggy tonight" is
almost never the median — it's the spikes. Every spike source has a specific
countermeasure:

- No heap allocation, no blocking I/O and no shared locks on the hot path;
  logging goes through a lock-free ring drained by an idle-priority thread.
- Reed-Solomon loss recovery runs NEON-vectorized, so the CPU cost of FEC
  lands precisely when the network is already misbehaving — several times
  faster than the scalar build other clients ship.
- Head-of-line waiting for out-of-order packets is capped at 2 ms (FEC
  already covers gaps) instead of the ~16 ms that is customary.
- CPU and GPU clocks are pinned for the session's duration, because a
  mostly-idle GPU gets downclocked by the governor and a "cheap" render pass
  quietly stops being cheap.

**It measures itself.** Per-frame timestamps at every stage produce p50/p99
summaries on-device — including the phase between frame arrival and the
panel's vsync, which drifts continuously (two free-running 60 Hz clocks) and
silently adds up to a frame of variance to any naive measurement. Latency
claims without percentiles and phase are folklore; this client ships its own
instruments.

## Controller-only mode

No video pipeline at all: the stream is negotiated at minimum profile to keep
the radio quiet, video packets are discarded before parsing, and the backlight
can switch off. What remains is a ~2 ms-staleness wireless controller with
gyro, touch and rumble — for when the PS5 is already on the TV and all you
want is the gamepad.

## Status

Feature-complete and building clean; **not yet validated on hardware**. The
first on-device session works through the verify list in
[docs/latency.md](docs/latency.md) — the design analysis behind every decision
above, including the honest limits (the display compositor's vblank latch is a
floor no userland client can beat).

## Building

The only host requirement is Docker; the toolchain is a pinned container.

```bash
scripts/build.sh
```

Output: `build/src/hayai.nro`. Deploy to a Switch running hbmenu's netloader
(press Y in hbmenu):

```bash
scripts/deploy.sh <switch-ip>
```

Logs and telemetry stream back over the same connection.

## Using it

First run: `Discover & register consoles` — you'll need your PSN account ID
(base64) and the 8-digit PIN from the PS5's Remote Play settings. Then select
the host and play. Config lives in `/config/hayai/hayai.conf`; it contains
pairing secrets, so don't share it. In-stream, hold **L+R+MINUS** for about a
second to return to the menu.

Latency experiment knobs (A/B without touching code):

```bash
cmake -B build -DHAYAI_REORDER_TIMEOUT_US=16000 -DHAYAI_FEEDBACK_MIN_MS=4 ...
```

## Layout

```
cmake/        toolchain + .nro packaging
toolchain/    pinned devkitPro build image
scripts/      build.sh, deploy.sh
shaders/      the NV12->RGB pass, compiled into romfs
docs/         latency.md - the full design analysis
src/
  app/        config, menus, discovery, registration, session orchestration
  core/       lock-free logging, telemetry, thread/core policy
  gfx/        deko3d device + 2-image immediate swapchain
  video/      NVDEC decoder + zero-copy renderer
  audio/      decode thread, audout, drift servo
  input/      2 ms JIT input sampler
  compat/     stubs that keep the vendored core lean
vendor/       protocol core + FEC/protobuf support libraries
```

## Licence & credits

AGPL-3.0-only (see `COPYING`). Binary releases must always be accompanied by
their source; publishing this repository satisfies that.

The Remote Play protocol implementation in `vendor/` is libchiaki, from the
[chiaki-ng](https://github.com/streetpea/chiaki-ng) project — the community
that reverse-engineered Remote Play in the open. Everything above the protocol
layer (`src/`, the pipelines described in this README) is original to hayai.
Exact upstreams, pinned commits and local modifications are documented in
[vendor/PROVENANCE.md](vendor/PROVENANCE.md); third-party licence texts are in
`LICENSES/`.

# hayai

The lowest-latency PlayStation 5 Remote Play client for Nintendo Switch
homebrew — plus a **controller-only mode** that turns the Switch into a
wireless gamepad for a PS5 on the TV.

*hayai* (速い — "fast") is built around one engineering goal: the shortest
possible path from a UDP datagram to a lit pixel, and from a button press to a
datagram. Every layer — network, decode, present, input, audio — is designed
for that goal, and instrumented so the claims below are measurable on the
device rather than asserted.

## Three things measurement caught that reading could not

Instrumentation earned its place immediately. Each of these was invisible in
normal use and obvious in the numbers:

- **An input flood was killing sessions.** The protocol library compares motion
  state with a 1e-7 epsilon — gyro noise exceeds that on every sample — and its
  minimum send interval is an unimplemented stub. Sampling at 2 ms therefore
  transmitted ~500 packets/second: 941 `ENOBUFS`, 31 send-buffer overflows, then
  a dead socket and 7605 consecutive errors. On Wi-Fi, uplink and downlink share
  airtime, so it was degrading the *video* too. Sends are now classed by what
  changed — buttons immediately, sticks past a noise threshold, motion on a
  cadence — and are roughly 4x fewer with *lower* button latency than before.
- **Acks were being discarded exactly when they mattered.** The library rejected
  every data ack whose size differed from the base header, but acks carrying
  gap-ack blocks are legitimately larger — and those are the ones sent while
  packets are being lost. 423 rejected in one session, so the send buffer never
  drained during congestion.
- **The reorder window was mistuned by an order of magnitude.** At 2 ms, routine
  Wi-Fi jitter was being classified as packet loss, each instance costing an FEC
  failure and an IDR round trip — far more visible than the wait it saved.

## Why it's fast

**Zero-copy video, end to end.** Frames are decoded by the Tegra X1's NVDEC
hardware into GPU memory and sampled *in place* by the display pass: the
decoder's own surfaces are aliased as GPU textures, and a single fragment
shader converts NV12 to RGB while drawing. The decoded image is never copied
and never touches the CPU. Typical clients pull every decoded frame back to
CPU memory and re-upload it as a texture — a two-way full-frame trip on a
mobile-class CPU, sixty times a second, that exists here as zero work.

**Decode on the receive thread, present off it.** The packet thread decrypts,
reassembles and decodes with no queue and no handoff, pinned alone on its own
CPU core — NVDEC is fixed-function and returns in about 1 ms, so that costs
nothing. Presentation is different: acquiring a display buffer blocks until the
compositor releases one, and doing that on the receive thread turns every
display stall into a networking stall. So decoded frames go to a **newest-wins
mailbox** and a dedicated thread presents them. A frame still waiting when the
next one decodes is dropped, never shown late.

**Present on decode, not on a schedule.** In the latency profile a finished
frame goes to the display the moment it exists, through a two-image swapchain
in immediate mode — it never waits for a render loop's next tick. Vsync with a
deeper swapchain is a profile choice, not a compile-time one.

**Input is classed by what a player can feel.** A dedicated thread samples
buttons, sticks, gyro and touch every 2 ms, so whatever is transmitted is
microseconds old rather than "whatever the UI loop last polled" — which at
60 Hz is up to 16.7 ms stale. But *sending* is deliberately not uniform:
button, trigger and touch changes go out immediately, stick movement is gated
by a noise threshold, and motion-only updates ride a fixed cadence. Treating
all three the same is how the flood described above happened.

**Audio is servo-controlled, not queue-managed.** The console's audio clock and
the Switch's DAC inevitably drift. Instead of letting a playback queue grow
(tens of milliseconds of creeping latency) and then flushing it (an audible
glitch), a control loop nudges a resampler by parts-per-million to hold the
buffer at a 30 ms setpoint. Audio gets a real jitter buffer where video does
not, because a late frame is an invisible repeat and a late audio buffer is an
audible click. Opus decoding runs on its own core, never on the network
thread.

**The latency tail is engineered, not hoped for.** "Feels laggy tonight" is
almost never the median — it's the spikes. Every spike source has a specific
countermeasure:

- No heap allocation, no blocking I/O and no shared locks on the hot path;
  logging goes through a lock-free ring drained by an idle-priority thread.
- Reed-Solomon loss recovery runs NEON-vectorized, so the CPU cost of FEC
  lands precisely when the network is already misbehaving — several times
  faster than the scalar build other clients ship.
- Head-of-line waiting for out-of-order packets is tuned per profile (2.5 ms
  to 10 ms) rather than the fixed ~16 ms that is customary — long enough to
  use a late packet, short enough that a lost one does not stall the pipeline.
- CPU and GPU clocks are pinned for the session's duration, because a
  mostly-idle GPU gets downclocked by the governor and a "cheap" render pass
  quietly stops being cheap.

**It measures itself.** Per-frame timestamps at every stage produce p50/p99
summaries on-device — including the phase between frame arrival and the
panel's vsync, which drifts continuously (two free-running 60 Hz clocks) and
silently adds up to a frame of variance to any naive measurement. Latency
claims without percentiles and phase are folklore; this client ships its own
instruments.

## Profiles

One choice sets resolution, bitrate, swapchain depth, how honestly congestion
is reported, and the reorder window together — so a session cannot silently end
up in an incoherent state.

| | Latency | Smooth | Quality |
|---|---|---|---|
| Resolution | 720p | 540p | 1080p |
| Bitrate | full | two thirds | full |
| Present | immediate, 2 buffers | vsync, 3 buffers | vsync, 3 buffers |
| Reorder window | 2.5 ms | 10 ms | 6 ms |
| Loss reported to console | capped 10% | honest, to 30% | capped 5% |

**Smooth** is the handheld pick. The console — not the client — chooses bitrate
during a session, switching between encode profiles based on the congestion
feedback we send it, so "adaptive" here means telling it the truth about the
link and leaving radio headroom rather than re-encoding anything.

## Controller-only mode

No video pipeline at all: the stream is negotiated at minimum profile to keep
the radio quiet, video packets are discarded before parsing, and the backlight
can switch off. What remains is a ~2 ms-staleness wireless controller with
gyro, touch and rumble — for when the PS5 is already on the TV and all you
want is the gamepad.

## Measured on hardware

From a 540p60 session on a real console and a real Wi-Fi link, reported by the
client's own instrumentation:

```
tl: 119 frames 59.5 fps | au->decoded 1.026/1.200 ms | au->presented 1.217/1.396 ms (p50/p99)
```

**1.2 ms from a complete frame to on screen, with p99 within 0.2 ms of p50.**
That is the entire client-side video cost: NVDEC decode, colour conversion and
present. Everything else in the latency budget belongs to the console's
encoder, the network, and the panel.

Both risks the design rested on are settled: the decoder's surfaces bind
directly as GPU textures (correct image, so the block-linear layout matches),
and no fence is needed before sampling them.

Still true, and worth stating: the display compositor latches at vblank, so
~8 ms of average display latency is a floor no userland client can beat.
[docs/latency.md](docs/latency.md) has the full analysis and the honest limits.

## Installing

Grab `hayai.nro` from [Releases](../../releases), drop it into `/switch/` on
your SD card, and launch it from hbmenu. No other files are needed — shaders
and assets are embedded.

A black screen for a few seconds after selecting a console is normal: that's
the session connecting, and video appears on the first decoded frame.

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

Consoles on the network appear on the main screen automatically, tagged ready,
standby or offline. Select an unpaired one to pair it — you'll need your PSN
account ID (base64) and the 8-digit PIN from **Settings > System > Remote Play
> Link Device** on the console. After that, selecting a paired console streams
it, and a console in standby is woken first. Config lives in
`/config/hayai/hayai.conf`; it contains
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

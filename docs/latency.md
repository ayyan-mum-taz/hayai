# hayai latency notes

Design analysis, pre-implementation. Everything here is from reading code and
known hardware behaviour; items marked **[verify]** need on-device numbers
before they are treated as true.

## The budget model

Glass-to-glass ≈ encode + network + reassembly + decode + present + panel.
Input ≈ HID sample staleness + send gating + network + console tick aliasing.

Two facts shape everything:

1. **The display latches at vblank regardless.** Swap interval 0 removes
   queueing, but buffers go through the vi compositor, which latches the newest
   buffer at each vsync. Display contributes ~8.3 ms avg / 16.7 worst, floor for
   userland. Mid-scanout flips would require driving nvdisp under the
   compositor (sysmodule; out of scope).
2. **The last packet of a frame gates decode.** Frame serialization time =
   frame_size / link_throughput. Bitrate, codec choice and IDR size are
   therefore *latency* parameters. HEVC ≈ 40% smaller frames at equal quality =
   latency win, fully hardware-decoded.

## Protocol-core findings (vendored chiaki-ng lib)

| Finding | Where | Consequence |
|---|---|---|
| AV reorder queue holds video packets up to **16 ms** when the head seq is missing | `takion.c:47` `TAKION_AV_REORDER_TIMEOUT_US`, `takion_av_queue_flush_with_timeout` | Upstream chiaki has no such queue; frameprocessor already handles out-of-order units within a frame (`put_unit` by index, flush-on-complete at `videoreceiver.c:184`). Plan: bypass or reduce to cross-frame-boundary ordering with ~2 ms budget. |
| **3 allocator ops per datagram** on the recv thread | `takion.c:1135` `malloc(1500)` (upstream's own `// TODO: no malloc?`), `realloc`, + AV entry `malloc` | 10-15k global-locked newlib malloc ops/sec = tail jitter. Replace with fixed slab ring. Pair with `recvmmsg` (libnx has it) to drain in one bsd-service IPC. |
| `video_sample_cb` runs **synchronously on the recv thread** | `streamconnection.c:416` | recv→decrypt→reassemble→NVDEC submit can be one thread, zero handoffs. Needs big `udp_rx_buf_size` (socketInitializeConfig) *and* `SO_RCVBUF`. Present handoff: measure both ways. |
| Feedback sender: state set by another thread, sent on cond wake, gated ≥ **8 ms** | `feedbacksender.c:5` | Staleness = poll age + gate wait. Restructure: sender thread samples HID itself at the send slot (JIT sampling). Motion changes every sample ⇒ state rate *is* motion rate: 8 ms = 125 Hz. Try 4 ms **[verify console tolerance]**. |
| `chiaki_cond_timedwait` is ms-granular | `thread.c` | ±1 ms quantization on the input gate; sender should own an absolute-time sleep loop. |
| AES keystream precomputed on bg thread, but underrun ⇒ **silent inline software AES on recv thread** | `gkcrypt.c` | Count underruns, size buffer so counter stays 0. |
| GMAC per packet, software; devkitPro `libmbedcrypto.a` contains **zero** `aese`/`aesmc` instructions (mbedTLS 2.x config, x86-only accel) | objdump | We build with `+crypto`; hand-rolled ARMv8 AES/GHASH possible. Second tier: measure first, scales with bitrate, lands as recv-thread jitter. |
| FEC failure ⇒ IDR request; IDR is several × P-frame size | `videoreceiver.c` flush path | Recovery frame serializes slowest right after loss. Argues for bitrate headroom below link capacity ("latency-first" preset), not at it. |

## Frontend design rules

- **No malloc, no printf, no lock shared with other threads on the hot path.**
  Our current `log_cb` does `printf`+`fflush` (nxlink = blocking TCP) and
  libchiaki logs from the recv thread — must become a lock-free ring drained by
  a low-prio thread *before* session work lands, or loss-event measurements are
  polluted.
- **Thread placement:** recv+decode top priority, own core; present on second
  core; UI/logging lowest on third. `svcSetThreadCoreMask`. Also makes
  measurements repeatable.
- **Decoded frames: mailbox, newest wins.** Never queue decoded frames; a
  jitter burst decodes both, presents newest.
- **Audio: PLL, not panic valve.** Fork strategy (SDL queue grows to ~83 ms,
  then flush) is a latency sawtooth + audible glitch. Ours: audout/audrv with
  2×~5 ms buffers, opus decodes into audout mempool, swresample ratio nudged by
  ppm from buffer-fill error. Steady ~10-15 ms, no glitches, absorbs PS5↔Switch
  clock drift permanently.
- **Applet vs application mode** changes memory and scheduling class; detect
  (`appletGetAppletType`) and display prominently. **[verify impact]**
- **Clock control** (`clkrst`/`pcv`): jitter/thermal-consistency lever, opt-in,
  battery cost.
- **Wi-Fi power save** buffers *downlink* at the AP; uplink (input) transmits
  immediately. Controller mode: button latency fine, rumble may lag — ok.

## Controller-only mode (correction)

Console decides what it sends; no official "no video" negotiation. Realistic
shape: request minimum profile (360p, min bitrate) + `disable_audio_video`
flag drops video pre-parse (`takion_handle_packet_av`). "Minimize and
discard", not "eliminate". Extras: backlight off via `lbl` (battery/OLED),
no render loop at all (GPU idle, thermal headroom, sustained clocks).

## Round 3 findings

| Finding | Evidence | Consequence |
|---|---|---|
| **FEC is scalar on a NEON CPU.** gf-complete ships NEON kernels (`src/neon/`) but the build never compiles them nor defines `ARM_NEON` (`gf_cpu.c:148` compiles detection out); chiaki-ng's CMake says `# TODO: support NEON` | vendor CMake + gf_cpu.c | RS recovery (runs exactly when packets were just lost) is 4-8x slower than it should be, on the recv thread, at the worst moment. Two lines of CMake. Cheapest real win so far. |
| **Opus decode runs on the video-critical thread.** Decoder registers as sink; `frame_cb` fires inside `chiaki_audio_receiver_av_packet`, called synchronously from the takion event path | `opusdecoder.c:36`, audioreceiver | ~0.5-1 ms stall injected into the video path every ~10 ms. Fix: SPSC ring of encoded packets (few hundred bytes) to the audio thread; decode next to audout submit. The one handoff that is architecturally correct. |
| **Sockets are IPC on Horizon, and the recv loop pays 2 per datagram** (`select` then `recvfrom`, `takion.c:1196`) | stoppipe/takion | ~6k+ IPC round trips/s. recvmmsg = structural, not nice-to-have. Hot loop becomes a software pipeline: burst-recv → assemble → send_packet (NVDEC async) → burst-recv again → receive_frame (already done) → present. Decode hides behind socket drains. |
| **Beat frequency: latency oscillates ~0-16.7 ms over minutes.** PS5 encode clock and Switch panel clock free-run; arrival phase vs vblank latch drifts continuously (period = minutes, set by crystal ppm delta) | physics | A/B tests must outlast a beat period or record phase. Telemetry: timestamp vi vsync event, log per-frame arrival phase; overlay shows beat position. Explains "some nights feel worse". Final justification for present-on-decode: fixed added delay only shifts which phase is lucky. |
| **DVFS trap: less work ⇒ slower.** Near-idle GPU downclocks; the ~0.3-0.6 ms pass stretches several-fold with governor jitter | Tegra DVFS | Pin low-but-sufficient fixed clocks during streaming (GPU, NVDEC, EMC). Reframes clock control: removing a variance source, not a turbo button. SwitchWave precedent. Decode time at 720p plausibly halvable from default NVDEC clock. |
| **Host-side latency lab.** Everything from takion inward is deterministic C given a packet trace | — | Device dumps header-only packet timelines; Mac harness replays through the real vendored code with fake decoder + seeded loss models. Reorder surgery, slab ring, FEC timing all A/B-able in CI without a console. Highest-leverage infra item while deployment is paused. |
| **ffmpeg is convenience, not necessity** (deferred end-state). `bitstream.c` already parses SPS/PPS + rewrites slice headers; RP streams are single-ref no-B | bitstream.c | Direct NVDEC (averne's hwaccel as the map) deletes parser layers (~100-300 µs, minor) but gains explicit completion fences for present scheduling. Modest latency, real control, large effort. Only if telemetry indicts the ffmpeg layer. |

**Principles codified:** No A/V-sync machinery ever (audio free-runs; skew < ~20 ms imperceptible; lip-sync logic is pure added latency). Quantitative triage: frame-assembly memcpy ≈ 10-30 µs and bitstream parse similar — not worth touching while ms-scale items exist.

**Audit items:** frameprocessor buffer prealloc (zero steady-state mallocs); MTU vs path-MTU + fragmentation warning; allowed thread-priority range under hbl vs NSP takeover; extend HID probe to sixaxis sampling rate.

## Verify-on-hardware list (ordered)

1. Zero-copy path: does averne's hwaccel wait for NVDEC completion before
   returning the frame, or is there a pending syncpt the renderer must wait on?
   Symptom if wrong: shimmer under load. kkwong's renderer doesn't wait
   (suggests decoder blocks — unconfirmed).
2. Block-linear layout match between NVDEC output and our deko3d ImageLayout
   (symptom: recognizable but block-scrambled image).
3. True HID report rate + worst gap (probe already written).
4. Feedback gate below 8 ms: does the console accept/act on it?
5. Reorder-queue bypass under real Wi-Fi loss: measure frame-completion time
   distribution with/without.
6. recvmmsg + slab ring: recv-thread time per packet before/after.
7. IP_TOS/DSCP on uplink: does the Wi-Fi driver map it to WMM ACs at all?
8. Immediate vs vsync present: measured delta (expect ~8 ms avg, tail-heavy).
9. Applet vs application mode: full telemetry comparison.
10. Docked + Ethernet adapter: the "is it the radio" control experiment.

## Measurement rig

No software on the Switch can see glass-to-glass. Rig: HUD flashes a white
square the instant a press is *sampled locally*; film Switch + TV at 240 fps
(4 ms resolution). Local-flash→TV-reaction = full round trip; flash also
isolates local input path. On-device: per-frame timeline (first packet, last
packet, decode done, present) in a ring, dumped over nxlink; percentiles, not
means — mush lives in the tail.

## Telemetry as a product feature

Protocol ack machinery already yields RTT/loss; stages carry timestamps
anyway. Live overlay + "network doctor" verdict ("link adds ~9 ms jitter;
5 GHz would likely fix") — no client in this niche explains *why* tonight
feels bad. Near-zero marginal cost.

## Presets

- **Latency-first (default):** 720p, HEVC, bitrate with headroom, immediate
  present, 4 ms input if tolerated.
- **Balanced / Quality:** documented tradeoffs, vsync opt-in.

## Build order — status after initial implementation

Built and compiling (unproven on hardware):

- [x] NEON FEC build fix (`vendor/CMakeLists.txt`)
- [x] Telemetry backbone: ring logger (`core/log`), per-frame percentiles +
      vsync-phase capture (`core/telemetry`), summaries off the hot path
- [x] Zero-handoff hot path: video cb = decode + render + present on the
      takion thread; takion pinned alone on core 1 via the
      `chiaki_thread_set_affinity_cb` hook (`core/thread`)
- [x] Opus off the recv thread: custom audio sink -> SPSC ring -> audio thread
      (`audio/`), drift PLL via swr compensation, small audout buffers
- [x] JIT-sampled input at 2 ms cadence with motion + touch (`input/`)
- [x] Reorder timeout 2 ms + feedback gate as build knobs (vendor `#ifndef`s)
- [x] Clock pinning CPU/GPU during stream (DVFS variance removal)
- [x] Service-side UDP buffer 1 MiB (`userAppInit`)
- [x] Controller-only mode: min profile + `CHIAKI_VIDEO_DISABLED` + backlight
- [x] Full app shell: discovery, registration, config, console menus

Still open, in order of value:

1. **Host-side replay harness** — packet-trace dump + Mac-side replay through
   the vendored core; makes reorder/slab surgery CI-testable.
2. recvmmsg burst loop + slab ring in takion (vendor surgery; needs harness).
3. Feedback gate 4 ms experiment (needs hardware to verify console tolerance).
4. DSCP/WMM uplink marking attempt.
5. Crypto acceleration if telemetry indicts GMAC.
6. Speculative, timeboxed: YUV overlay plane via vi; direct-NVDEC path only if
   the ffmpeg layer is indicted.

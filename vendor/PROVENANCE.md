# Vendored code

| Directory | Upstream | Pinned at | Licence |
|---|---|---|---|
| `chiaki-lib/` | [chiaki-ng](https://github.com/streetpea/chiaki-ng) `lib/` | `6547d8aed03503646fe1043512616e26c03fa9db` | AGPL-3.0-only (with OpenSSL exception) |
| `nanopb/` | [nanopb](https://github.com/nanopb/nanopb) | `cad3c18ef15a663e30e3e43e3a752b66378adec1` | Zlib |
| `jerasure/` | [streetpea/jerasure](https://github.com/streetpea/jerasure) | `505ccb4bc69eee8d7ea40a6089a056b99671134f` | BSD-3-Clause |
| `gf-complete/` | [streetpea/gf-complete](https://github.com/streetpea/gf-complete) | `fa54a4670a5705c84abf6c24b92b0cd479625478` | BSD-3-Clause |

## Why chiaki-ng and not upstream chiaki

Upstream `~thestr4ng3r/chiaki` is in maintenance mode and its author points at
chiaki-ng as the continuation. chiaki-ng's `lib/` carries the newer PS5 work
(codec negotiation, `bitstream.c` SPS/PPS parsing, RUDP) which is what we build
the low-latency path on top of. Its *Switch frontend* is explicitly stale --
chiaki-ng's own `scripts/switch/README.md` says the Switch build "will
essentially end up with the same as existing chiaki" -- so we take the protocol
core and write the frontend ourselves.

## Local modifications

`chiaki-lib/CMakeLists.txt` is ours. The trim it applies is documented at the
top of that file; the summary is that `src/remote/holepunch.c` is swapped for
`../../src/compat/holepunch_stub.c` (7 stub symbols) so that libcurl, json-c,
miniupnpc and libevent are not needed for LAN play.

Source changes under `chiaki-lib/` (kept to `#ifndef` guards so a rebase is a
trivial re-apply):

| File | Change | Why |
|---|---|---|
| `src/takion.c` | `TAKION_AV_REORDER_TIMEOUT_US` wrapped in `#ifndef` | Build knob `HAYAI_REORDER_TIMEOUT_US` (default 2000 vs upstream 16000): FEC already covers intra-frame gaps, the 16 ms head-of-line wait is mostly added latency |
| `src/feedbacksender.c` | `FEEDBACK_STATE_TIMEOUT_MIN_MS` wrapped in `#ifndef` | Build knob `HAYAI_FEEDBACK_MIN_MS` (default 8): this gate is also the motion-data rate; 4 ms is the experiment once console tolerance is verified |

Build-level (no source diff): gf-complete is compiled **with** its NEON
kernels and `ARM_NEON` defined (`vendor/CMakeLists.txt`) — chiaki-ng builds
these scalar, which makes Reed-Solomon loss recovery several times slower at
exactly the moment the link is degraded.

## Licence consequence

libchiaki is AGPL-3.0-only, so hayai as a whole is AGPL-3.0-only. That is
intended: this is meant to be an open source project.

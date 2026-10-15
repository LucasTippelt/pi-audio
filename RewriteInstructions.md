Change list for the Pi Audio DSP Engine

Items are grouped by theme, with priority in brackets: P0 = foundational, decide before more code is written; P1 = fix soon; P2 = before the product ships.

Rewrite Everything into new Folders pi-audio-dsp-rewrite and pi-audio-web-rewrite

A. Architecture

A1. Split into DSP core, HAL and control plane [P0] [updated]

DSP core: a portable library whose entry point is process(in, out, nframes), plus a command/telemetry interface (A3). It has no OS calls and no file I/O, and its portability rules are in A4.
HAL: the HAL owns the audio thread and calls Engine::onBlock() once per block with planar float buffers and a BlockInfo (sample index, host time, frames, clock state). The engine does not own the loop. This callback model maps directly onto a DMA/ISR backend later.
Control plane: config, reload, state files, monitoring and networking. It talks to the core only through the command/telemetry interface.
Why: the current playback thread owns timing, test-mode file reads, shm reads and network sends. That is the part that is expensive to untangle when the processor changes, and the split makes the DSP unit-testable offline.

A2. Decide the target platform class early [P0]

Embedded Linux SoC: stay on ALSA, with a PREEMPT_RT kernel, mlockall, isolated cores and IRQ affinity.
Bare-metal/RTOS DSP or MCU: no ALSA. Process in the I2S/SAI DMA interrupt, with hardware ASRC for async inputs.
Hybrid (Linux for network/UI, dedicated DSP or MCU for audio): the audio engine is no longer a Linux process. shm rings, the monitor writer, relay, HTTP/WebSocket servers, JSON config and the control-file test mode all become Linux-side clients of the DSP.
Why: this determines how much of today’s code survives. The HAL (A1) and the message interface (A3) are what keep all three options open. With a Linux SoC, ALSA stays as the interface to the vendor’s ASoC driver and only the device config changes.

A3. Message-based interface between engine and DSP core [P0] [new]
The engine never calls into the core directly or shares state with it.

Commands in: small fixed-size structs with a versioned header (type, sequence number, optional “apply at sample N”). Examples: set gain, mute, load stage parameters, schedule at sample N, enter/leave test mode.
Telemetry out: a fixed-size status block (xruns, clock state, meters, latency, firmware/core version), polled or pushed.
Parameters as compact binary blocks, not JSON. The Linux side parses JSON, validates it, and precomputes filter coefficients. It sends the DSP a versioned parameter set. An MCU should not parse JSON or run libm-heavy math.
Transport: in development, an in-process lock-free queue on the Pi. In production, the same structs serialized over SPI, UART, USB, RPMsg/shared memory or Ethernet, with a checksum on the link.
Core side: the core applies commands at block boundaries, using the double-buffered stage swap and the apply-at-sample timestamps described in I2.
Why: “DSP on a remote chip” then becomes just a different transport for the same queue, and the Pi build stays a faithful stand-in for the product.

A4. Portability constraints on the DSP core [P0/P1] [new]

Memory: no heap allocation after init, no exceptions or RTTI, no std::string or std::vector in the processing path, fixed maximum block size.
Numerics: single-precision float only, with no accidental double. Confirm the target has a hardware FPU. If the target is fixed-point, the whole core needs a different numeric strategy, so decide early.
Budgets: a written cycle and memory budget per stage. A Pi 4 hides inefficiencies that a Cortex-M7 or DSP won’t forgive, so log per-stage timing next to maxCallbackUs and compare it against the target’s budget.
Dependencies: no Linux-side headers. Check that <atomic> and any other C++ library features exist in the target toolchain.
Math: avoid libm-heavy calls (sin, pow, exp) in the block path. Precompute coefficients on the control side.
Early silicon check: the Pi can validate algorithms, protocol and HAL contract, but not ISR latency, DMA jitter, cache effects or real cycle counts. Run the core with a trivial HAL on the candidate silicon (an eval board is enough) before the architecture is fixed.

A5. HAL implementations and test ladder [P1] [new]

SimHal: no ALSA. It runs faster than real time from files or generated signals, and injects synthetic clock offsets (for example +30 ppm) and lock/unlock/holdover events. Use it in CI to unit-test the DSP, the drift controller, the xrun fades and lock-loss behaviour.
AlsaHal + null device (VM): timer pacing. Verifies thread structure, shm plumbing and the control plane.
AlsaHal + real interface (Pi): device pacing, linked capture/playback, real xrun behaviour and snd_pcm_delay latency.
Later, LinkHal or a virtual link between engine instances in the VM: to develop the chain protocol before any AES hardware exists.
ALSA specifics stay inside AlsaHal: format conversion (S32_LE/S24_3LE to float), interleave/deinterleave, xrun recovery, linked streams, RT priority.
VM pacing is a HAL option, auto-detected via snd_pcm_type() == SND_PCM_TYPE_NULL, with a config override. A safety net switches to timer pacing if writei repeatedly returns in under half a period, which catches plug:-wrapped null devices.

B. Clocking

B1. One master clock: the converter clock [P0] [updated]

On real hardware the device wakes the engine (blocking writei or snd_pcm_wait/poll, period 128–256 frames, 2–3 periods).
Timer pacing exists only as a Timer mode inside AlsaHal for null devices.
If the chain clock is recovered from an AES input and drives the card’s I2S, the device is still the master and nothing changes in the engine.

B2. Capture and playback on one card, in one thread [P0]

Link the streams with snd_pcm_link. Each cycle: read mics, run DSP, write output. Use snd_pcm_delay() for the fixed latency offset.
Why: mics and speaker output become sample-aligned, which measurement needs.

B3. Drift compensation for anything on a different clock [P0]

This covers shm PGM and mics in the VM, a console feeding the head speaker with an unrelated clock, and the network.
Monitor the ring fill level, run a PI controller or DLL to estimate the ratio (1 ± tens of ppm), and feed a fractional resampler. References: zita-ajbridge, alsa_in. A hardware ASRC is the alternative on the product.
A PGM source abstraction selects between DeviceChannels (sample-synchronous) and ShmAsync (with the resampler).

B4. Explicit xrun/overrun policy [P1]

Underrun: output silence with a short fade, re-prime with silence, never a bare snd_pcm_prepare(). Overrun: drop with a fade.
Count both and expose the counters through telemetry (A3) and engine-state.json.

B5. Avoid multiple cards in the audio path [P1]

Separate cards mean separate clocks and extra ASRC. Use one interface for mics and outputs during development.

B6. Clock state as a first-class input [P1] [new]

The HAL reports Internal/Locked/Holdover/Unlocked through BlockInfo. A non-RT helper thread polls the card’s lock/clock-source control via snd_ctl and publishes it through an atomic.
The core fades and mutes when the clock isn’t Locked or Internal, and never runs with an unknown clock.
C. Real-time thread hygiene

C1. Make the playback thread truly I/O-free [P0]

Remove the per-cycle read of /dev/shm/pi-audio-test-mode. A background thread watches it and publishes the mode through an atomic (later, as a test-mode command via A3).
Test-injection shm reads move to a background thread that feeds in-memory SPSC rings.
Why: mmap reads can page-fault. mlockall plus pre-faulting helps, but “zero file I/O” isn’t currently true.

C2. No network syscalls from the RT thread [P0]

The HTTP and WebSocket servers currently send() from the playback thread. Push to a ring and let a normal-priority thread send, as the relay already does. Check that no mutex is shared with the accept/client threads.

C3. No allocation or deallocation on the RT thread [P1]

This applies to stage swaps and config reloads. Retire old objects through a queue to a background thread. A3 and A4 make this structural.

C4. Rework thread priorities [P1]

Use roughly 70–80 instead of SCHED_FIFO “max” (HAL default rtPriority = 75), and consider core pinning.
Capture and the PGM/mic reader threads are in the signal path, so give them lower-priority SCHED_FIFO.

C5. Nothing calls into the control plane from the RT thread [P1] [new]

Clock state travels in BlockInfo, counters sit in Stats/telemetry, and the state-file writer polls them.
D. Monitoring and networking

D1. Consolidate to one monitoring path [P1] [updated]

Today there are four: monitor shm, TCP relay to Node, embedded HTTP stream and embedded WebSocket. Pick one and serve it from a separate process.
In the hybrid product these become Linux-side clients of the DSP, fed by audio returned over the link, not by an embedded server in the audio process.
Why: it removes the HTTP/WebSocket/SHA-1 code and its attack surface from the process hosting the RT thread.
E. Shared memory, IPC and config

E1. Define shm lifecycle and ownership [P1]

Needing rm /dev/shm/pi-audio-test-* after “write failed” shows the lifecycle is unclear. Add a version/generation field and a handshake, and define who creates, resets and unlinks each object.

E2. Treat shm headers and indices as untrusted [P1]

Bounds-check read and write positions and validate magic and version before use.

E3. JSON stays on the Linux side [P1] [new]

JSON parsing, validation and coefficient calculation live in the control plane. The core receives only versioned binary parameter blocks (A3).
The reload trigger and 5 s polling are replaced by “stage parameters, confirm, apply at sample N” over the command interface (I4).
shm rings are a development-time transport and not part of the product audio path.
F. DSP quality and protection

F1. Parameter smoothing [P1]

Ramp volume and mute to avoid clicks and zipper noise. Atomics alone only make the change safe, not inaudible.

F2. Denormal handling [P1]

Enable FTZ/DAZ (or add DC offsets) for IIR stages. Check that the target supports an equivalent.

F3. Safety limiting [P1 for test noise, P2 for the product]

A limiter on the white-noise test signal now. Full driver protection (excursion and thermal limiting) in the product chain, designed in from the start.

F4. Declared latency per stage [P1] [new]

Every stage declares a fixed latency, and the total is reported in telemetry. Chain alignment (I1) depends on it.

G. Measurement

G1. Keep timing relationships for sweeps and deconvolution [P1]

Solved by B2 (same device, same clock). Otherwise timestamp every block. Across speakers it requires the shared timebase (I2). Mic data returns over the control/Ethernet path, not AES3.

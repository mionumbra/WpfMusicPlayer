# FAPO Equalizer Prototype Design

## Status

This design records the direction approved on 2026-07-10:

- keep the current decoder, equalizer-worker, playback-worker, queues, FIFO, and locking model;
- keep `volume=0.7` in the libavfilter graph;
- remove the libavfilter equalizer and limiter stages;
- run a ten-band biquad equalizer followed by a dynamic limiter inside one custom FAPO on the FAudio source voice;
- preserve the existing ten-band public API while keeping the DSP model ready for a future fully parametric equalizer.

## Goals

1. Prove that the audible equalizer and limiter can run in FAudio without changing the current application-side pipeline topology.
2. Preserve the existing ten fixed center frequencies, `Q=1`, gain range `[-24, 24]` dB, and `GetEqualizerBand`/`SetEqualizerBand` interfaces.
3. Keep coefficient generation and the DSP core independent of FAudio so a future XAPO adapter or fully parametric controls do not require rewriting the signal processing.
4. Keep the FAPO real-time path deterministic: no allocation, blocking, logging, managed calls, or contended locks in `Process` or `Reset`.
5. Retain the current limiter policy as closely as is useful for a prototype: linked channels, ceiling `0.70`, 5 ms look-ahead, 50 ms release, and no automatic make-up gain.

## Non-goals

- Removing libavfilter, the equalizer worker, decoded-frame queue, PCM FIFO, or playback worker.
- Adding UI or public APIs for frequency, Q, filter type, limiter settings, or an arbitrary band count.
- Implementing a native Windows XAPO adapter in this change.
- Bit-identical output with FFmpeg's `equalizer` or `alimiter` implementations.
- Moving the FFT analyzer into the FAPO real-time path. The spectrum remains a pre-FAPO view in this prototype.
- Smoothing coefficient changes. A gain update takes effect atomically at a FAudio processing boundary; click-free automation can be added later without changing the public API.

## Target pipeline

```text
decoder worker
  -> decoded AVFrame queue
  -> existing equalizer worker
  -> libavfilter: abuffer -> aresample(stereo/s16) -> volume(0.7)
                  -> aformat(stereo/s16) -> abuffersink
  -> existing AVAudioFifo
  -> playback worker
  -> FAudio SourceVoice (PCM s16 input; FAudio converts effects to float32)
  -> custom FAPO: parametric biquad chain -> linked look-ahead limiter
  -> mastering voice
```

The equalizer worker remains the owner of the conversion graph and continues to feed the FIFO exactly as it does now. Only the graph nodes and the destination of equalizer parameter updates change.

## Component boundaries

### Pure DSP and control model

Create a focused native unit, tentatively `EqualizerDsp.h/.cpp`, with no dependency on FAudio, FFmpeg, C++/CLI, or UI types.

Its control model has a fixed ten-slot array for this prototype. Each slot is represented as a parametric band definition containing:

- filter type;
- enabled state;
- center frequency in Hz;
- Q;
- gain in dB.

Only the peaking filter type is implemented initially. The current public interface populates the ten slots with `31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000` Hz, `Q=1`, and the stored gains. The type/frequency/Q fields form the future full-parametric extension point; exposing them later must not change the processing core.

The control side computes normalized RBJ coefficients in double precision and stores float coefficients in an immutable, fixed-size, trivially-copyable runtime snapshot. The snapshot contains an ABI version, byte size, reset generation, enabled-band mask, and ten normalized coefficient sets; it contains no pointers, dynamic containers, strings, or platform `bool` fields. A band is converted to an exact identity section when its gain is zero or when `2 * frequency >= sampleRate`.

The run-time equalizer uses transposed direct form II, with one state pair per channel and band. It consumes interleaved float32 frames and supports both in-place and out-of-place processing.

### Dynamic limiter

The same pure DSP unit owns a limiter that runs after all biquads. It is linked across channels so one channel's peak applies the same gain to every channel and does not shift the stereo image.

Prototype constants are fixed for the effect lifetime:

- ceiling: `0.70`;
- look-ahead: `5 ms`;
- release: `50 ms`;
- make-up/auto level: disabled.

`prepare(sampleRate, channelCount, maxFrameCount)` allocates the delay line and peak-tracking storage before real-time processing starts. To match the current FFmpeg setting, the look-ahead window is `max(1, floor(sampleRate * 0.005))` frames and the content delay is one frame shorter: 239 frames at 48 kHz and 219 frames at 44.1 kHz. For each post-EQ frame, the limiter computes the maximum absolute sample across channels and inserts that linked peak into a preallocated monotonic window. When the delay line is full, it outputs the oldest frame using `targetGain = min(1, 0.70 / windowPeak)`. A lower target takes effect immediately because the delayed peak is already known; an increasing target follows a linear release step chosen to return from the last attenuation to unity in 50 ms. A final safety clamp enforces `[-0.70, 0.70]` despite floating-point error.

This definition produces approximately 5 ms of algorithmic delay, comparable to the current FFmpeg limiter setting, while keeping per-frame peak lookup amortized O(1) at sample rates up to 192 kHz.

Silent input is treated as zero without reading its buffer. The DSP continues producing delayed samples and decaying biquad state while a tail exists. After the delay line has received zeros for one complete content-delay interval and every biquad state/output magnitude is below `1e-8`, it clears those states and reports silence. Explicit stop, seek, repeat, and pipeline reset increment a reset generation in the parameter snapshot; the next processing pass clears biquad and limiter history before consuming new audio.

Exact FFmpeg envelope trajectories are not required, but the ceiling, linked-channel behavior, look-ahead duration, release direction, and no-make-up policy are required and tested.

### FAPO adapter

Create `FapoEqualizer.h/.cpp` as a thin adapter around the pure DSP unit. It uses the installed `FAPOBase` helpers and owns three fixed-size parameter blocks.

The adapter responsibilities are limited to:

- validate one float32 input and one float32 output with matching rate and channel count in `LockForProcess`;
- prepare all DSP storage during `LockForProcess`;
- acquire a stable parameter snapshot with `FAPOBase_BeginProcess`;
- fully overwrite out-of-place output buffers and correctly handle in-place buffers;
- implement disabled pass-through;
- propagate `ValidFrameCount` and set `FAPO_BUFFER_VALID` until the DSP tail is exhausted;
- validate the exact parameter-block byte size before forwarding `SetParameters`;
- clear DSP history in `Reset` without allocating;
- release all storage only outside `Process`.

The adapter advertises support for in-place processing but does not require it. The FAPO contains no references to `MusicPlayerNative`, managed objects, the FFT analyzer, or libavfilter.

### MusicPlayer integration

`initialize_audio_engine` creates the SourceVoice as it does now, then explicitly attaches one enabled FAPO using `FAudioVoice_SetEffectChain`. The effect is attached explicitly rather than passed to `FAudio_CreateSourceVoice`, because the installed FAudio implementation does not propagate an effect-chain setup failure from `CreateSourceVoice`.

The factory reference is released after `SetEffectChain`; a successful voice attachment owns the remaining reference. A first full-size `FAudioVoice_SetEffectParameters` call is made before the SourceVoice starts, preallocating FAudio's parameter staging buffer so later same-size updates do not allocate. Existing teardown order remains SourceVoice, mastering voice, then FAudio, so destroying the SourceVoice unlocks and releases the FAPO.

`SetEqualizerBand` continues to:

1. reject an invalid band index;
2. clamp gain to `[-24, 24]`;
3. update the stored ten-band state under the existing `filter_graph_mutex`;
4. build a complete coefficient snapshot outside the FAudio real-time thread;
5. call `FAudioVoice_SetEffectParameters` for effect index zero when a SourceVoice exists.

FAudio copies the submitted fixed-size snapshot and applies it before the next `Process` call. Production code never calls the FAPO parameter method directly.

The existing `filter_graph_mutex` also serializes the equalizer control snapshot with SourceVoice effect attachment, parameter publication, and teardown. This closes the pointer-lifetime window without adding another application mutex; the FAPO real-time thread never takes it.

The libavfilter graph removes:

- all ten `equalizer` filter contexts and their links;
- the `alimiter` context and link;
- the associated per-band graph bookkeeping and run-time `avfilter_graph_send_command` call.

The graph retains `abuffer`, `aresample`, `volume=0.7`, `aformat`, and `abuffersink`. Thread creation, queue ownership, EOF flushing, mutexes, and condition variables remain unchanged.

## Error handling and lifecycle

- FAPO allocation, format locking, or effect-chain attachment failure makes audio-engine initialization fail and follows the existing cleanup path.
- Invalid parameter sizes keep the last valid snapshot; debug builds may assert, but the audio thread must not throw.
- `Process` never throws across the FAPO ABI.
- Effect initialization accepts the project's configured sample-rate range and skips bands at or above Nyquist.
- A reset-generation change clears delayed samples, peak history, envelope state, and biquad history at the start of a processing pass. This prevents pre-seek or pre-repeat audio leaking into the next stream segment.
- Explicit stop continues to truncate the effect tail, matching the current stop/flush behavior. Natural EOF may drain the short FAPO tail before the effect reports silence.

## Tests

Add a separate native test executable project, `MusicPlayerLibrary.NativeTests`, which compiles the same `EqualizerDsp.cpp` and `FapoEqualizer.cpp` sources used by the production library. The test executable must not create an audio device and must return a non-zero exit code on any failed assertion. No test-only type, method, export, or friend-assembly declaration is added to the production `MusicPlayerLibrary` DLL.

Required offline tests are:

1. Ten zero-dB bands produce an identity response before the limiter.
2. A 48 kHz, 1 kHz, `Q=1`, `+6 dB` band produces approximately `+6 dB` steady-state RMS gain at its center, within `0.1 dB`.
3. Bands at or above Nyquist become identity sections and never produce NaN or infinity.
4. The existing public gain interface clamps values to `[-24, 24]`.
5. The limiter keeps steady and transient output within `0.70` plus a small floating-point tolerance.
6. A peak in either stereo channel applies the same gain to both channels.
7. Silent input drains the look-ahead buffer and eventually reports no tail.
8. A reset generation removes all previous biquad and limiter history.
9. In-place and out-of-place processing produce equivalent results.
10. Disabled FAPO processing is an exact pass-through.
11. At 48 kHz the first non-zero impulse sample is delayed by 239 frames; at 44.1 kHz it is delayed by 219 frames.
12. Splitting identical input into different processing-quantum sizes produces identical output.

After the test cycle, validate the complete `WpfMusicPlayer.slnx` x64 Debug build with the repository's `build-wpfmusicplayer` skill script. Run `MusicPlayerLibrary.NativeTests.exe` and the existing MSTest project separately because solution compilation builds but does not execute either test suite.

## Known prototype limitations

- The application still has three audio worker threads and the same queue/lock topology, so this stage does not yet deliver the synchronization reduction discussed in the feasibility analysis.
- The FFT visualizer receives PCM before the FAPO and therefore shows the pre-EQ/pre-limiter spectrum.
- The FAPO limiter is behaviorally compatible at the policy level, not sample-for-sample identical to FFmpeg `alimiter`.
- Parameter changes are quantum-boundary atomic but not smoothed.
- Only FAudio/FAPO is implemented; a future native XAudio2 backend needs a separate XAPO adapter sharing the same DSP core.

## Rejected alternatives

1. **All logic inline in `MusicPlayerLibrary.cpp`:** fewer files, but DSP, ABI handling, and pipeline lifecycle become inseparable and the future parametric EQ is harder to test.
2. **FAudio's built-in FAPOFX equalizer:** it does not provide the required ten-band behavior and is not a reliable implementation base in the installed/current FAudio source.
3. **Keep the limiter in libavfilter:** it would run before the FAPO equalizer and could not enforce the final post-EQ ceiling.

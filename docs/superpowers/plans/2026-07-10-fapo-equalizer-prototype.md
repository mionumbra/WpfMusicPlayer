# FAPO Equalizer Prototype Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the libavfilter equalizer and limiter nodes with a testable ten-band parametric DSP core and linked look-ahead limiter hosted by one custom FAPO, while retaining the existing public ten-band API and application thread/queue topology.

**Architecture:** `EqualizerDsp` is a pure C++ unit that compiles semantic parametric-band settings into fixed-size coefficient snapshots and runs TDF-II biquads followed by a linked limiter. `FapoEqualizer` is a thin FAPOBase adapter. A separate native executable compiles those exact production sources for offline tests, so the production C++/CLI DLL contains no test-only interfaces.

**Tech Stack:** C++20, C++/CLI on .NET 10, FAudio 26.6 FAPO/FAPOBase, FFmpeg 8.1.2 libavfilter, a native assertion-based test executable, existing MSTest 4.0.2 tests, Visual Studio 2026 MSBuild/v145.

## Global Constraints

- Keep decoder, equalizer-worker, playback-worker, decoded-frame queue, AVAudioFifo, mutexes, and condition variables.
- Keep `volume=0.7` in libavfilter; remove libavfilter `equalizer` and `alimiter` processing.
- Preserve `GetEqualizerBand(int)`, `SetEqualizerBand(int, int)`, ten center frequencies, `Q=1`, and `[-24, 24]` dB clamp.
- FAPO order is ten parametric biquads followed by a linked limiter with ceiling `0.70`, 5 ms look-ahead, 50 ms linear release, and no make-up gain.
- `FAPO::Process`, `FAPO::Reset`, and the called DSP methods must not allocate, block, log, call managed code, throw, or acquire an application mutex.
- Keep the FFT tap before FAPO for this prototype.
- Support configured sample rates through 192 kHz; bands at or above Nyquist compile as identity while retaining their semantic gain.
- Do not add native XAPO, public full-parametric controls, coefficient smoothing, or arbitrary band counts.
- Put all new DSP/FAPO internals tests in `MusicPlayerLibrary.NativeTests`; do not add test-only types/methods/exports to `MusicPlayerLibrary`.
- Preserve the user's unrelated five modified WPF files and never stage them with feature commits.

---

### Task 1: Native test project and RBJ biquad core

**Files:**
- Create: `MusicPlayerLibrary/EqualizerDsp.h`
- Create: `MusicPlayerLibrary/EqualizerDsp.cpp`
- Create: `MusicPlayerLibrary.NativeTests/MusicPlayerLibrary.NativeTests.vcxproj`
- Create: `MusicPlayerLibrary.NativeTests/NativeTests.cpp`
- Modify: `MusicPlayerLibrary/MusicPlayerLibrary.vcxproj`
- Modify: `WpfMusicPlayer.slnx`
- Modify: `WpfMusicPlayer.ARM64.slnx`

**Interfaces:**
- Produces: `AudioDsp::MakeDefaultTenBandConfig()`
- Produces: `AudioDsp::CompileEqualizerSnapshot(const EqualizerConfig&, uint32_t, uint64_t)`
- Produces: `EqualizerDsp::Prepare/Process/Reset/HasTail/GetLimiterDelayFrames`
- Produces: `MusicPlayerLibrary.NativeTests.exe [eq|limiter|fapo|all]`

- [ ] **Step 1: Write RED tests and test runner**

Create a native console project with x64/ARM64 Debug/Release, toolset v145, C++20, `VcpkgEnableManifest=true`, `VcpkgManifestRoot=$(SolutionDir)`, and output directory `$(SolutionDir)$(Platform)\$(Configuration)\`. Add it to both `.slnx` files.

Start `NativeTests.cpp` with a small real-code runner:

```cpp
#include "../MusicPlayerLibrary/EqualizerDsp.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace
{
    using namespace MusicPlayerLibrary::AudioDsp;

    void Require(bool condition, std::string_view message)
    {
        if (!condition)
            throw std::runtime_error(std::string(message));
    }

    void RequireClose(float expected, float actual, float tolerance,
                      std::string_view message)
    {
        if (std::abs(expected - actual) > tolerance)
            throw std::runtime_error(std::string(message));
    }

    std::vector<float> StereoSine(
        std::uint32_t rate, float frequency, float amplitude, std::uint32_t frames)
    {
        std::vector<float> result(frames * 2);
        for (std::uint32_t frame = 0; frame < frames; ++frame)
        {
            const float sample = amplitude * std::sin(
                2.0f * std::numbers::pi_v<float> * frequency * frame / rate);
            result[frame * 2] = result[frame * 2 + 1] = sample;
        }
        return result;
    }

    void TestZeroDbIdentity();
    void TestOneKilohertzPlusSixDb();
    void TestNyquistAndConfiguredRates();

    struct TestCase { const char* group; const char* name; void (*run)(); };
    const TestCase Tests[] = {
        {"eq", "zero dB identity", TestZeroDbIdentity},
        {"eq", "1 kHz +6 dB", TestOneKilohertzPlusSixDb},
        {"eq", "Nyquist and configured rates", TestNyquistAndConfiguredRates}
    };
}

int main(int argc, char** argv)
{
    const std::string_view selected = argc > 1 ? argv[1] : "all";
    int failed = 0;
    for (const auto& test : Tests)
    {
        if (selected != "all" && selected != test.group)
            continue;
        try { test.run(); std::cout << "PASS " << test.name << '\n'; }
        catch (const std::exception& error)
        {
            ++failed;
            std::cerr << "FAIL " << test.name << ": " << error.what() << '\n';
        }
    }
    return failed == 0 ? 0 : 1;
}
```

The three test functions must assert:

- zero gains reproduce a deterministic stereo signal within `1e-6`;
- band index 5 at 1 kHz, Q=1, +6 dB produces `6.0 ± 0.1` dB RMS after 4096 warm-up frames;
- sample rates `8000, 11025, 16000, 22050, 44100, 48000, 88200, 96000, 192000` remain finite, and the 16 kHz band is identity at a 16 kHz sample rate.

- [ ] **Step 2: Verify RED**

Run:

```powershell
& "C:\Users\madoka\.codex\skills\build-wpfmusicplayer\scripts\build.ps1" -RepositoryRoot "<worktree>" -Platform x64 -Configuration Debug -SkipRestore -SkipVcpkgInstall
```

Expected: the native test project fails because `EqualizerDsp.h/.cpp` do not exist.

- [ ] **Step 3: Implement the pure DSP interface**

Create `EqualizerDsp.h` with:

```cpp
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

namespace MusicPlayerLibrary::AudioDsp
{
    inline constexpr std::size_t EqualizerBandCount = 10;
    inline constexpr std::array<float, EqualizerBandCount>
        EqualizerBandFrequenciesHz{
            31.0f, 62.0f, 125.0f, 250.0f, 500.0f,
            1000.0f, 2000.0f, 4000.0f, 8000.0f, 16000.0f
        };
    inline constexpr std::uint32_t EqualizerSnapshotAbiVersion = 1;

    enum class BiquadType : std::uint32_t { Peaking = 0 };
    struct EqualizerBandConfig
    {
        BiquadType type = BiquadType::Peaking;
        float frequency_hz = 1000.0f;
        float q = 1.0f;
        float gain_db = 0.0f;
        bool enabled = true;
    };
    struct EqualizerConfig
    {
        std::array<EqualizerBandConfig, EqualizerBandCount> bands{};
    };
    struct BiquadCoefficients
    {
        float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f;
        float a1 = 0.0f, a2 = 0.0f;
    };
    struct EqualizerDspSnapshot
    {
        std::uint32_t abi_version = EqualizerSnapshotAbiVersion;
        std::uint32_t byte_size = 0;
        std::uint64_t reset_generation = 0;
        std::uint32_t enabled_mask = 0;
        std::uint32_t reserved = 0;
        std::array<BiquadCoefficients, EqualizerBandCount> bands{};
    };
    struct LimiterConfig
    {
        bool enabled = true;
        float ceiling = 0.70f;
        float lookahead_ms = 5.0f;
        float release_ms = 50.0f;
    };
    static_assert(std::is_trivially_copyable_v<EqualizerDspSnapshot>);

    EqualizerConfig MakeDefaultTenBandConfig() noexcept;
    EqualizerDspSnapshot CompileEqualizerSnapshot(
        const EqualizerConfig&, std::uint32_t, std::uint64_t) noexcept;

    class EqualizerDsp final
    {
    public:
        bool Prepare(std::uint32_t, std::uint32_t, std::uint32_t,
                     const LimiterConfig&);
        void Reset() noexcept;
        bool Process(const EqualizerDspSnapshot&, const float*, float*,
                     std::uint32_t, bool) noexcept;
        [[nodiscard]] bool HasTail() const noexcept;
        [[nodiscard]] std::uint32_t GetLimiterDelayFrames() const noexcept;
    private:
        struct BiquadState { float z1 = 0.0f, z2 = 0.0f; };
        std::uint32_t sample_rate_ = 0;
        std::uint32_t channel_count_ = 0;
        std::uint32_t max_frame_count_ = 0;
        std::uint64_t applied_reset_generation_ = ~std::uint64_t{0};
        LimiterConfig limiter_{};
        std::vector<BiquadState> biquad_states_;
    };
}
```

`EqualizerDsp.cpp` must be self-contained and must not include `pch.h`, allowing the native test project to compile the exact production source. Add it to the C++/CLI project with `PrecompiledHeader=NotUsing`.

Add `..\MusicPlayerLibrary\EqualizerDsp.cpp` as a linked `ClCompile` item in the native test project; do not copy the implementation.

Compile peaking coefficients from the W3C/RBJ formula in double precision, normalize by a0, and store float. Set `snapshot.byte_size=sizeof(EqualizerDspSnapshot)` after construction. Zero gain, invalid Q, disabled, or `2*f >= sampleRate` is an exact identity with its enabled bit cleared.

Use transposed direct form II:

```cpp
const float y = c.b0 * x + state.z1;
state.z1 = c.b1 * x - c.a1 * y + state.z2;
state.z2 = c.b2 * x - c.a2 * y;
x = y;
```

Task 1 implements the limiter-disabled path; `GetLimiterDelayFrames()` returns zero.

- [ ] **Step 4: Verify GREEN and commit**

Build x64 Debug, then run:

```powershell
.\x64\Debug\MusicPlayerLibrary.NativeTests.exe eq
```

Expected: all EQ cases pass, exit code 0.

Commit only Task 1 files:

```powershell
git add -- MusicPlayerLibrary/EqualizerDsp.h MusicPlayerLibrary/EqualizerDsp.cpp MusicPlayerLibrary/MusicPlayerLibrary.vcxproj MusicPlayerLibrary.NativeTests/MusicPlayerLibrary.NativeTests.vcxproj MusicPlayerLibrary.NativeTests/NativeTests.cpp WpfMusicPlayer.slnx WpfMusicPlayer.ARM64.slnx
git commit -m "feat(audio): add testable parametric EQ core"
```

---

### Task 2: Linked look-ahead limiter

**Files:**
- Modify: `MusicPlayerLibrary/EqualizerDsp.h`
- Modify: `MusicPlayerLibrary/EqualizerDsp.cpp`
- Modify: `MusicPlayerLibrary.NativeTests/NativeTests.cpp`

**Interfaces:**
- Consumes: Task 1 snapshot and DSP class.
- Produces: limiter-enabled, chunk-invariant processing with 239-frame delay at 48 kHz and 219-frame delay at 44.1 kHz.

- [ ] **Step 1: Write RED limiter tests**

Add native tests for:

1. An impulse at frame 0 appears at frame 239 (48 kHz) and frame 219 (44.1 kHz).
2. Input frame `(1.0, 0.25)` becomes approximately `(0.70, 0.175)`, proving stereo linking and ceiling enforcement.
3. A 0.5 signal remains 0.5 after delay, proving no make-up gain.
4. After a 1.0 peak followed by 0.5, gain rises from 0.7 to 1 over 2400 frames at 48 kHz; first release increment is `0.3/2400`.
5. Silent input drains a queued impulse and eventually clears `HasTail()`.
6. Incrementing `reset_generation` before silent processing drops queued history.
7. One-block and 127-frame-chunk processing produce samples equal within `1e-6`.

Register these cases under group `limiter`.

- [ ] **Step 2: Verify RED**

Build and run `MusicPlayerLibrary.NativeTests.exe limiter`. Expected: delay is zero and ceiling/linking assertions fail.

- [ ] **Step 3: Implement preallocated limiter**

Add preallocated delay and monotonic-peak state:

```cpp
struct PeakNode { std::uint64_t frame_index = 0; float peak = 0.0f; };
std::uint32_t lookahead_window_frames_ = 1;
std::uint32_t limiter_delay_frames_ = 0;
std::uint64_t limiter_frame_index_ = 0;
std::uint64_t silent_input_frames_ = 0;
std::size_t delay_write_frame_ = 0;
std::size_t peak_head_ = 0, peak_tail_ = 0;
float limiter_gain_ = 1.0f;
float limiter_release_step_ = 0.0f;
std::vector<float> delay_line_;
std::vector<PeakNode> peak_queue_;
```

`Prepare` allocates:

```cpp
lookahead_window_frames_ = (std::max)(
    1u, static_cast<std::uint32_t>(
        std::floor(sample_rate * limiter.lookahead_ms / 1000.0f)));
limiter_delay_frames_ = limiter.enabled ? lookahead_window_frames_ - 1u : 0u;
delay_line_.assign(
    static_cast<std::size_t>(lookahead_window_frames_) * channel_count, 0.0f);
peak_queue_.assign(lookahead_window_frames_ + 1u, {});
```

`Reset` clears these allocations with `std::fill` and never resizes.

Per frame: read and filter every channel into `std::array<float,64>`, compute one linked peak, maintain the monotonic window, output the delayed frame, apply `target=min(1,0.70/windowPeak)`, reduce immediately, recover linearly in 50 ms, and safety-clamp to ±0.70. Read all channels before overwriting in-place memory.

Treat silent input as zero without reading its pointer. Return valid while output/internal state is at least `1e-8`; after a full silent delay and all state is below threshold, clear state and return false.

- [ ] **Step 4: Verify GREEN and commit**

Build and run groups `eq` and `limiter`; both exit 0.

```powershell
git add -- MusicPlayerLibrary/EqualizerDsp.h MusicPlayerLibrary/EqualizerDsp.cpp MusicPlayerLibrary.NativeTests/NativeTests.cpp
git commit -m "feat(audio): add linked lookahead limiter"
```

---

### Task 3: Custom FAPO adapter

**Files:**
- Create: `MusicPlayerLibrary/FapoEqualizer.h`
- Create: `MusicPlayerLibrary/FapoEqualizer.cpp`
- Modify: `MusicPlayerLibrary/MusicPlayerLibrary.vcxproj`
- Modify: `MusicPlayerLibrary.NativeTests/MusicPlayerLibrary.NativeTests.vcxproj`
- Modify: `MusicPlayerLibrary.NativeTests/NativeTests.cpp`

**Interfaces:**
- Produces: `uint32_t CreateEqualizerFapo(const EqualizerDspSnapshot&, const LimiterConfig&, FAPO**) noexcept`.
- Supports one matching float32 input/output, in-place and out-of-place, valid/silent flags, disabled pass-through, exact-size parameter snapshots.

- [ ] **Step 1: Write RED FAPO tests**

Add group `fapo` tests that call the real FAPO vtable directly:

- in-place and NaN-prefilled out-of-place output match within `1e-6`;
- disabled+VALID input is exact pass-through;
- SILENT input backed by NaN memory is never read and yields finite output;
- a parameter block of `sizeof(EqualizerDspSnapshot)-1` leaves parameters unchanged;
- `LockForProcess` rejects mismatched channels or non-float32 format.

- [ ] **Step 2: Verify RED**

Build. Expected: `FapoEqualizer.h` and `CreateEqualizerFapo` are missing.

- [ ] **Step 3: Implement FAPOBase adapter**

Create:

```cpp
[[nodiscard]] std::uint32_t CreateEqualizerFapo(
    const EqualizerDspSnapshot& initial,
    const LimiterConfig& limiter,
    FAPO** effect) noexcept;
```

The object begins with `FAPOBase`, then three fixed snapshot blocks, `EqualizerDsp`, limiter config, and channel count. Use one input/output and flags:

```cpp
FAPO_FLAG_CHANNELS_MUST_MATCH |
FAPO_FLAG_FRAMERATE_MUST_MATCH |
FAPO_FLAG_BITSPERSAMPLE_MUST_MATCH |
FAPO_FLAG_BUFFERCOUNT_MUST_MATCH |
FAPO_FLAG_INPLACE_SUPPORTED
```

`LockForProcess` validates one matching extensible IEEE-float32 input/output and calls `Prepare`. `SetParameters` rejects wrong size/byte_size/ABI before calling the base. `Reset` only clears DSP state.

Disabled processing must copy VALID out-of-place input, but for SILENT input it must not read the buffer and must zero an out-of-place output. Enabled processing gets a stable snapshot with `FAPOBase_BeginProcess`, runs DSP, fully writes output, sets `FAPO_BUFFER_VALID` until tail ends, then calls `EndProcess`.

Factory allocation uses `new(std::nothrow)`, initializes all three blocks, and returns reference count one. No callbacks throw across the ABI. Both new `.cpp` files are self-contained/PCH-disabled so the native tests compile the same sources.

- [ ] **Step 4: Verify GREEN and commit**

Build and run `MusicPlayerLibrary.NativeTests.exe all`; exit 0.

```powershell
git add -- MusicPlayerLibrary/FapoEqualizer.h MusicPlayerLibrary/FapoEqualizer.cpp MusicPlayerLibrary/MusicPlayerLibrary.vcxproj MusicPlayerLibrary.NativeTests/MusicPlayerLibrary.NativeTests.vcxproj MusicPlayerLibrary.NativeTests/NativeTests.cpp
git commit -m "feat(audio): add custom equalizer FAPO"
```

---

### Task 4: Player integration and libavfilter shrink

**Files:**
- Modify: `MusicPlayerLibrary/MusicPlayerLibrary.h`
- Modify: `MusicPlayerLibrary/MusicPlayerLibrary.cpp`
- Create: `WpfMusicPlayer.Test/FapoEqualizerApiTest.cs`

**Interfaces:**
- Consumes: `CreateEqualizerFapo` and `CompileEqualizerSnapshot`.
- Preserves: existing managed/native ten-band getter/setter signatures.
- Produces: `abuffer -> aresample -> volume=0.7 -> aformat -> abuffersink`.

- [ ] **Step 1: Record RED structural evidence**

Run before editing:

```powershell
rg -n -S 'avfilter_get_by_name\("(equalizer|alimiter)"\)|avfilter_graph_send_command|filter_graphs|limiter_ctx' .\MusicPlayerLibrary
```

Expected: current equalizer/alimiter/filter command references are present.

Before changing production code, add and run this characterization test for the API that must remain stable:

```csharp
using MusicPlayerLibrary;

namespace WpfMusicPlayer.Test;

[TestClass]
public sealed class FapoEqualizerApiTest
{
    [TestMethod]
    public void ExistingTenBandApi_ClampsGain()
    {
        using var player = new MusicPlayer();
        player.SetEqualizerBand(0, 100);
        Assert.AreEqual(24, player.GetEqualizerBand(0));
        player.SetEqualizerBand(0, -100);
        Assert.AreEqual(-24, player.GetEqualizerBand(0));
    }
}
```

Run it before editing and record that it passes on the old implementation; it is a characterization guard, while the `rg` result is the RED evidence for DSP migration.

- [ ] **Step 2: Replace graph EQ bookkeeping**

Replace `eq_bands`, `av_filter_eq_graph`, `filter_graphs`, and `limiter_ctx` with:

```cpp
AudioDsp::EqualizerConfig equalizer_config =
    AudioDsp::MakeDefaultTenBandConfig();
std::uint64_t equalizer_reset_generation = 0;
AVFilterGraph* filter_graph = nullptr;
AVFilterContext* filter_context_src = nullptr;
AVFilterContext* filter_context_sink = nullptr;
AVFilterContext* resample_ctx = nullptr;
AVFilterContext* volume_ctx = nullptr;
AVFilterContext* format_normalize_ctx = nullptr;

AudioDsp::EqualizerDspSnapshot
    build_equalizer_snapshot_locked() const noexcept;
bool publish_equalizer_snapshot_locked() noexcept;
```

Remove the old constructor assignment and duplicate band/Nyquist constants.

- [ ] **Step 3: Attach and prewarm FAPO**

In `initialize_audio_engine`:

1. Compile an initial snapshot under `filter_graph_mutex`.
2. Create SourceVoice into a local pointer with null chain.
3. Create one FAPO; explicitly call/check `FAudioVoice_SetEffectChain` because the installed `CreateSourceVoice` ignores internal chain failure.
4. Release the factory FAPO reference after `SetEffectChain`.
5. Re-lock, compile the latest snapshot, call `FAudioVoice_SetEffectParameters` once to preallocate staging, then publish the SourceVoice member.
6. Destroy local resources outside the mutex on failure.

Limiter constants are `{enabled=true, ceiling=.70f, lookahead_ms=5, release_ms=50}`.

During teardown, move `source_voice` to a local and null the member under `filter_graph_mutex`; stop/flush/destroy after unlocking.

- [ ] **Step 4: Publish snapshots from existing API**

```cpp
AudioDsp::EqualizerDspSnapshot
MusicPlayerLibrary::MusicPlayerNative::build_equalizer_snapshot_locked() const noexcept
{
    return AudioDsp::CompileEqualizerSnapshot(
        equalizer_config, sample_rate, equalizer_reset_generation);
}

bool MusicPlayerLibrary::MusicPlayerNative::publish_equalizer_snapshot_locked() noexcept
{
    if (!source_voice)
        return true;
    const auto snapshot = build_equalizer_snapshot_locked();
    return FAudioVoice_SetEffectParameters(
        source_voice, 0, &snapshot, sizeof(snapshot), FAUDIO_COMMIT_NOW) == 0;
}
```

Getter reads semantic gain under the existing mutex. Setter validates index, clamps to ±24, stores gain, publishes a full snapshot, and logs caller-thread publication failure while retaining the control value.

After decoder/filter activity stops in `reset_audio_context`, increment reset generation and publish before workers restart. Never call `FAPO::Reset` directly.

- [ ] **Step 5: Remove avfilter DSP nodes**

Delete EQ/limiter creation, links, commands, structures, and cleanup. Link:

```cpp
if ((ret = avfilter_link(filter_context_src, 0, resample_ctx, 0)) < 0 ||
    (ret = avfilter_link(resample_ctx, 0, volume_ctx, 0)) < 0 ||
    (ret = avfilter_link(volume_ctx, 0, format_normalize_ctx, 0)) < 0 ||
    (ret = avfilter_link(format_normalize_ctx, 0, filter_context_sink, 0)) < 0)
    return fail_filter_with_ffmpeg(ret);
```

Keep worker names/topology unchanged and update comments that incorrectly call FIFO data post-EQ.

- [ ] **Step 6: Verify GREEN and commit**

Build x64 Debug. Run native tests and the existing MSTest suite:

```powershell
.\x64\Debug\MusicPlayerLibrary.NativeTests.exe all
dotnet test .\WpfMusicPlayer.Test\WpfMusicPlayer.Test.csproj -c Debug --no-build
```

Run the Task 4 `rg` command again; expected no matches. Confirm `volume=0.7`, `SetEffectChain`, and `SetEffectParameters` remain.

```powershell
git add -- MusicPlayerLibrary/MusicPlayerLibrary.h MusicPlayerLibrary/MusicPlayerLibrary.cpp WpfMusicPlayer.Test/FapoEqualizerApiTest.cs
git commit -m "feat(audio): route equalizer and limiter through FAPO"
```

---

### Task 5: Full verification and handoff

**Files:**
- Verify: `WpfMusicPlayer.slnx`
- Verify: `MusicPlayerLibrary.NativeTests`
- Verify: `WpfMusicPlayer.Test`
- Verify: all feature files

- [ ] **Step 1: Fresh complete build**

Run without skip flags:

```powershell
& "C:\Users\madoka\.codex\skills\build-wpfmusicplayer\scripts\build.ps1" -RepositoryRoot "<worktree>" -Platform x64 -Configuration Debug
```

Record restore status, solution/platform/configuration, exit code, warning count, and error count.

- [ ] **Step 2: Run both test suites**

```powershell
.\x64\Debug\MusicPlayerLibrary.NativeTests.exe all
dotnet test .\WpfMusicPlayer.Test\WpfMusicPlayer.Test.csproj -c Debug --no-build
```

Record exact passed/failed counts and both exit codes.

- [ ] **Step 3: Static and real-time safety verification**

```powershell
rg -n -S 'avfilter_get_by_name\("(equalizer|alimiter)"\)|avfilter_graph_send_command|filter_graphs|limiter_ctx' .\MusicPlayerLibrary
rg -n -S 'volume=0\.7|FAudioVoice_SetEffectChain|FAudioVoice_SetEffectParameters' .\MusicPlayerLibrary\MusicPlayerLibrary.cpp
git diff --check
git status --short
```

Inspect `Process`/`Reset` call paths and confirm they contain no allocation, resize, mutex, wait, log, managed call, or throw; coefficient trigonometry only appears in snapshot compilation.

- [ ] **Step 4: Final report**

Report FAPO placement/order, retained avfilter stages, public API compatibility, unchanged worker topology, exact build/test evidence, FFT remaining pre-FAPO, and deferred smoothing/native-XAPO work. Do not claim synchronization reduction at this stage.

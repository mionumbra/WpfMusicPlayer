// SPDX-License-Identifier: MIT

#pragma once

namespace MusicPlayerLibrary
{
	struct AudioPipelineBufferingProfile
	{
		double small_fft_microseconds = 0.0;
		int fifo_target_milliseconds = 140;
		int decoded_queue_target_milliseconds = 32;
	};

	// 做一个256-point FFT运算，根据运算耗时来选择FIFO水位
	[[nodiscard]] AudioPipelineBufferingProfile SelectAudioPipelineBufferingProfile(
		double small_fft_microseconds) noexcept;

	// Called by NativeLibraryRuntime::Initialize so the benchmark is complete
	// before the first MusicPlayer instance is constructed.
	void PrecomputeAudioPipelineBufferingProfile() noexcept;

	[[nodiscard]] const AudioPipelineBufferingProfile&
		GetAudioPipelineBufferingProfile() noexcept;
}

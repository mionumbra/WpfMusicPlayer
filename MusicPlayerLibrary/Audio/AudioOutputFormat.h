// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <string>

#include <FAudio.h>

#if defined(__cplusplus)
extern "C" {
#endif
#include <libavutil/samplefmt.h>
#if defined(__cplusplus)
}
#endif

namespace MusicPlayerLibrary
{
	enum class AudioChannelMode : int
	{
		System = 0,
		Mono = 1,
		Stereo = 2,
		Surround51 = 3,
		Surround71 = 4
	};

	enum class AudioBitDepth : int
	{
		System = 0,
		Bit16 = 16,
		Bit32 = 32
	};

	struct AudioOutputFormat
	{
		// A zero sample rate and System enum values request the current default
		// rendering-device format.
		int requested_sample_rate = 0;
		AudioChannelMode requested_channel_mode = AudioChannelMode::System;
		AudioBitDepth requested_bit_depth = AudioBitDepth::System;

		int sample_rate = 0;
		std::uint16_t channel_count = 0;
		std::uint32_t channel_mask = 0;
		AudioBitDepth bit_depth = AudioBitDepth::System;
		AVSampleFormat sample_format = AV_SAMPLE_FMT_NONE;
		std::string ffmpeg_channel_layout;
		FAudioWaveFormatExtensible wave_format{};
	};

	// 根据显式指定的设备格式进行解析。将其设为公共方法，以便无需打开音频设备即可测试格式映射。
	AudioOutputFormat ResolveAudioOutputFormat(
		const AudioOutputFormat& requested,
		const FAudioWaveFormatExtensible& system_format);

	// 根据请求的配置，解析当前设备能提供的输出格式
	AudioOutputFormat ResolveAudioOutputFormat(
		const AudioOutputFormat& requested = {});
}

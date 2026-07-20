// SPDX-License-Identifier: MIT

#include "pch.h"

#include "Audio/AudioOutputFormat.h"

#include <bit>
#include <cmath>
#include <cstring>
#include <stdexcept>

#if defined(__cplusplus)
extern "C" {
#endif
#include <libavutil/channel_layout.h>
#if defined(__cplusplus)
}
#endif

namespace
{
	using namespace MusicPlayerLibrary;
	constexpr FAudioGUID PcmSubFormat{
		0x00000001, 0x0000, 0x0010,
		{0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}
	};
	constexpr FAudioGUID IeeeFloatSubFormat{
		0x00000003, 0x0000, 0x0010,
		{0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}
	};

	bool SameGuid(const FAudioGUID& left, const FAudioGUID& right) noexcept
	{
		return std::memcmp(&left, &right, sizeof(FAudioGUID)) == 0;
	}

	std::uint32_t DefaultChannelMask(const int channels) noexcept
	{
		switch (channels)
		{
		case 1: return SPEAKER_MONO;
		case 2: return SPEAKER_STEREO;
		case 6: return SPEAKER_5POINT1_SURROUND;
		case 8: return SPEAKER_7POINT1_SURROUND;
		default: return 0;
		}
	}

	std::string DescribeChannelLayout(const std::uint32_t mask, const int channels)
	{
		switch (channels)
		{
		case 1:
			if (mask == SPEAKER_MONO) return "mono";
			break;
		case 2:
			if (mask == SPEAKER_STEREO) return "stereo";
			break;
		case 6:
			if (mask == SPEAKER_5POINT1_SURROUND) return "5.1(side)";
			break;
		case 8:
			if (mask == SPEAKER_7POINT1_SURROUND) return "7.1";
			break;
		default:
			break;
		}

		AVChannelLayout layout{};
		if (mask != 0 && av_channel_layout_from_mask(&layout, mask) == 0)
		{
			char description[128]{};
			if (av_channel_layout_describe(&layout, description, sizeof(description)) >= 0)
			{
				av_channel_layout_uninit(&layout);
				return description;
			}
			av_channel_layout_uninit(&layout);
		}

		av_channel_layout_default(&layout, channels);
		char description[128]{};
		if (layout.nb_channels == channels &&
			av_channel_layout_describe(&layout, description, sizeof(description)) >= 0)
		{
			av_channel_layout_uninit(&layout);
			return description;
		}
		av_channel_layout_uninit(&layout);
		throw std::invalid_argument("Unsupported system audio channel layout");
	}

	FAudioWaveFormatExtensible FallbackSystemFormat() noexcept
	{
		FAudioWaveFormatExtensible result{};
		result.Format.wFormatTag = FAUDIO_FORMAT_EXTENSIBLE;
		result.Format.nChannels = 2;
		result.Format.nSamplesPerSec = 48000;
		result.Format.wBitsPerSample = 32;
		result.Format.nBlockAlign = 8;
		result.Format.nAvgBytesPerSec = 384000;
		result.Format.cbSize = sizeof(FAudioWaveFormatExtensible) - sizeof(FAudioWaveFormatEx);
		result.Samples.wValidBitsPerSample = 32;
		result.dwChannelMask = SPEAKER_STEREO;
		result.SubFormat = IeeeFloatSubFormat;
		return result;
	}

}

MusicPlayerLibrary::AudioOutputFormat MusicPlayerLibrary::ResolveAudioOutputFormat(
	const AudioOutputFormat& requested,
	const FAudioWaveFormatExtensible& system_format)
{
	AudioOutputFormat result = requested;
	const FAudioWaveFormatEx& system_wfx = system_format.Format;

	result.sample_rate = requested.requested_sample_rate > 0
		? requested.requested_sample_rate
		: static_cast<int>(system_wfx.nSamplesPerSec);
	if (result.sample_rate <= 0)
		result.sample_rate = 48'000;

	switch (requested.requested_channel_mode)
	{
	case AudioChannelMode::Mono:
		result.channel_count = 1;
		result.channel_mask = SPEAKER_MONO;
		result.ffmpeg_channel_layout = "mono";
		break;
	case AudioChannelMode::Stereo:
		result.channel_count = 2;
		result.channel_mask = SPEAKER_STEREO;
		result.ffmpeg_channel_layout = "stereo";
		break;
	case AudioChannelMode::Surround51:
		result.channel_count = 6;
		result.channel_mask = SPEAKER_5POINT1_SURROUND;
		result.ffmpeg_channel_layout = "5.1(side)";
		break;
	case AudioChannelMode::Surround71:
		result.channel_count = 8;
		result.channel_mask = SPEAKER_7POINT1_SURROUND;
		result.ffmpeg_channel_layout = "7.1";
		break;
	case AudioChannelMode::System:
		result.channel_count = system_wfx.nChannels;
		if (result.channel_count == 0)
			result.channel_count = 2;
		result.channel_mask = system_format.dwChannelMask;
		if (result.channel_mask == 0 ||
			std::popcount(result.channel_mask) != result.channel_count)
		{
			result.channel_mask = DefaultChannelMask(result.channel_count);
		}
		result.ffmpeg_channel_layout = DescribeChannelLayout(
			result.channel_mask, result.channel_count);
		break;
	default:
		throw std::invalid_argument("Unsupported audio channel mode");
	}

	if (requested.requested_bit_depth == AudioBitDepth::System)
	{
		const bool system_is_float = system_wfx.wFormatTag == FAUDIO_FORMAT_IEEE_FLOAT ||
			(system_wfx.wFormatTag == FAUDIO_FORMAT_EXTENSIBLE &&
				SameGuid(system_format.SubFormat, IeeeFloatSubFormat));
		std::uint16_t system_valid_bits = system_wfx.wBitsPerSample;
		if (system_wfx.wFormatTag == FAUDIO_FORMAT_EXTENSIBLE &&
			SameGuid(system_format.SubFormat, PcmSubFormat) &&
			system_format.Samples.wValidBitsPerSample > 0 &&
			system_format.Samples.wValidBitsPerSample <= system_wfx.wBitsPerSample)
		{
			system_valid_bits = system_format.Samples.wValidBitsPerSample;
		}
		result.bit_depth = system_is_float
			? AudioBitDepth::Bit32
			: system_valid_bits <= 16
				? AudioBitDepth::Bit16
				: system_valid_bits <= 24
					? AudioBitDepth::Bit24
					: AudioBitDepth::Bit32;
	}
	else
	{
		result.bit_depth = requested.requested_bit_depth;
	}

	switch (result.bit_depth)
	{
	case AudioBitDepth::Bit16:
		result.sample_format = AV_SAMPLE_FMT_S16; break;
	case AudioBitDepth::Bit24:
		// FFmpeg has no packed 24-bit sample format. Keep 24 significant bits in
		// an S32 container so the FIFO, FAudio frame size, and FFT input agree.
		result.sample_format = AV_SAMPLE_FMT_S32; break;
	case AudioBitDepth::Bit32:
		result.sample_format = AV_SAMPLE_FMT_FLT; break;
	default:
		throw std::invalid_argument("Unsupported audio bit depth");
	}

	const bool use_float = result.bit_depth == AudioBitDepth::Bit32;
	const std::uint16_t container_bits = result.bit_depth == AudioBitDepth::Bit24
		? 32
		: static_cast<std::uint16_t>(result.bit_depth);
	const std::uint16_t valid_bits = static_cast<std::uint16_t>(result.bit_depth);
	FAudioWaveFormatExtensible& wave_format = result.wave_format;
	wave_format = {};
	wave_format.Format.wFormatTag = FAUDIO_FORMAT_EXTENSIBLE;
	wave_format.Format.nChannels = result.channel_count;
	wave_format.Format.nSamplesPerSec = result.sample_rate;
	wave_format.Format.wBitsPerSample = container_bits;
	wave_format.Format.nBlockAlign = static_cast<std::uint16_t>(
		result.channel_count * container_bits / 8);
	wave_format.Format.nAvgBytesPerSec =
		wave_format.Format.nSamplesPerSec * wave_format.Format.nBlockAlign;
	wave_format.Format.cbSize = sizeof(FAudioWaveFormatExtensible) - sizeof(FAudioWaveFormatEx);
	wave_format.Samples.wValidBitsPerSample = valid_bits;
	wave_format.dwChannelMask = result.channel_mask;
	wave_format.SubFormat = use_float ? IeeeFloatSubFormat : PcmSubFormat;
	return result;
}

MusicPlayerLibrary::AudioOutputFormat MusicPlayerLibrary::ResolveAudioOutputFormat(
	const AudioOutputFormat& requested)
{
	FAudioWaveFormatExtensible system_format = FallbackSystemFormat();
	FAudio* query_engine = nullptr;
	if (FAudioCreate(&query_engine, 0, FAUDIO_DEFAULT_PROCESSOR) == FAUDIO_OK)
	{
		FAudioDeviceDetails details{};
		if (FAudio_GetDeviceDetails(query_engine, 0, &details) == FAUDIO_OK)
			system_format = details.OutputFormat;
		FAudio_Release(query_engine);
	}
	return ResolveAudioOutputFormat(requested, system_format);
}

int MusicPlayerLibrary::GetAudioChannelTypeId(
	const int channel_count,
	const std::uint64_t channel_mask) noexcept
{
	switch (channel_count)
	{
	case 1:
		if (channel_mask == 0 || channel_mask == SPEAKER_MONO)
			return static_cast<int>(AudioChannelMode::Mono);
		break;
	case 2:
		if (channel_mask == 0 || channel_mask == SPEAKER_STEREO)
			return static_cast<int>(AudioChannelMode::Stereo);
		break;
	case 6:
		if (channel_mask == SPEAKER_5POINT1 ||
			channel_mask == SPEAKER_5POINT1_SURROUND)
		{
			return static_cast<int>(AudioChannelMode::Surround51);
		}
		break;
	case 8:
		if (channel_mask == SPEAKER_7POINT1 ||
			channel_mask == SPEAKER_7POINT1_SURROUND)
		{
			return static_cast<int>(AudioChannelMode::Surround71);
		}
		break;
	default:
		break;
	}
	return static_cast<int>(AudioChannelMode::Unknown);
}

MusicPlayerLibrary::AudioFormatInfo MusicPlayerLibrary::GetAudioFormatInfo(
	const AudioOutputFormat& format) noexcept
{
	return {
		GetAudioChannelTypeId(format.channel_count, format.channel_mask),
		format.sample_rate,
		static_cast<int>(format.bit_depth)
	};
}

double MusicPlayerLibrary::CalculateAudioBitrateKBytesPerSecond(
	const std::uint64_t encoded_bytes,
	const double decoded_duration_seconds) noexcept
{
	if (encoded_bytes == 0 || decoded_duration_seconds <= 0.0 ||
		!std::isfinite(decoded_duration_seconds))
	{
		return 0.0;
	}
	return static_cast<double>(encoded_bytes) /
		decoded_duration_seconds / 1000.0;
}

void MusicPlayerLibrary::AudioBitrateTracker::Reset() noexcept
{
	encoded_bytes_ = 0;
	decoded_duration_seconds_ = 0.0;
	kbytes_per_second_.store(0.0, std::memory_order_relaxed);
}

void MusicPlayerLibrary::AudioBitrateTracker::ObserveEncodedBytes(
	const std::uint64_t encoded_bytes) noexcept
{
	encoded_bytes_ += encoded_bytes;
}

void MusicPlayerLibrary::AudioBitrateTracker::ObserveDecodedSamples(
	const int sample_count,
	const int sample_rate) noexcept
{
	if (sample_count <= 0 || sample_rate <= 0)
		return;

	decoded_duration_seconds_ += static_cast<double>(sample_count) / sample_rate;
	kbytes_per_second_.store(
		CalculateAudioBitrateKBytesPerSecond(
			encoded_bytes_, decoded_duration_seconds_),
		std::memory_order_relaxed);
}

double MusicPlayerLibrary::AudioBitrateTracker::GetKBytesPerSecond() const noexcept
{
	return kbytes_per_second_.load(std::memory_order_relaxed);
}

double MusicPlayerLibrary::CalculateAverageAudioBitrateBitsPerSecond(
	const std::uint64_t stream_length_bytes,
	const double duration_seconds) noexcept
{
	if (stream_length_bytes == 0 || duration_seconds <= 0.0 ||
		!std::isfinite(duration_seconds))
	{
		return 0.0;
	}
	return static_cast<double>(stream_length_bytes) * 8.0 / duration_seconds;
}

bool MusicPlayerLibrary::IsLoselessAudio(
	const double average_bitrate_bits_per_second) noexcept
{
	return std::isfinite(average_bitrate_bits_per_second) &&
		average_bitrate_bits_per_second > 800000.0;
}

bool MusicPlayerLibrary::IsHiResAudio(const int source_sample_rate) noexcept
{
	return source_sample_rate > 48000;
}

// SPDX-License-Identifier: MIT

#include "pch.h"

#include <algorithm>
#include <stdexcept>

#include "Audio/Pipeline/Device/FAudioOutputDevice.h"

namespace
{
	std::mutex SharedDeviceMutex;
	std::shared_ptr<MusicPlayerLibrary::FAudioOutputDevice> SharedDevice;
	bool SharedDeviceShutdown = false;

	FAudioWaveFormatExtensible FallbackDeviceFormat() noexcept
	{
		FAudioWaveFormatExtensible result{};
		result.Format.wFormatTag = FAUDIO_FORMAT_IEEE_FLOAT;
		result.Format.nChannels = 2;
		result.Format.nSamplesPerSec = 48'000;
		result.Format.nAvgBytesPerSec = 48'000 * 2 * sizeof(float);
		result.Format.nBlockAlign = 2 * sizeof(float);
		result.Format.wBitsPerSample = 32;
		result.dwChannelMask = SPEAKER_STEREO;
		return result;
	}
}

MusicPlayerLibrary::FAudioOutputDevice::FAudioOutputDevice(
	const AudioOutputFormat& requested)
{
	if (FAudioCreate(&engine_, 0, FAUDIO_DEFAULT_PROCESSOR) != FAUDIO_OK || !engine_)
		throw std::runtime_error("FAudioCreate failed");
	try
	{
		FAudioDeviceDetails details{};
		system_format_ =
			FAudio_GetDeviceDetails(engine_, 0, &details) == FAUDIO_OK
			? details.OutputFormat
			: FallbackDeviceFormat();
		output_format_ = ResolveAudioOutputFormat(requested, system_format_);

		if (FAudio_CreateMasteringVoice(
			engine_,
			&mastering_voice_,
			output_format_.channel_count,
			output_format_.sample_rate,
			0,
			0,
			nullptr) != FAUDIO_OK || !mastering_voice_)
		{
			throw std::runtime_error("FAudio_CreateMasteringVoice failed");
		}
	}
	catch (...)
	{
		if (mastering_voice_)
			FAudioVoice_DestroyVoice(mastering_voice_);
		mastering_voice_ = nullptr;
		FAudio_Release(engine_);
		engine_ = nullptr;
		throw;
	}
}

std::shared_ptr<MusicPlayerLibrary::FAudioOutputDevice>
MusicPlayerLibrary::FAudioOutputDevice::Acquire(const AudioOutputFormat& requested)
{
	std::lock_guard lock(SharedDeviceMutex);
	if (SharedDeviceShutdown)
		throw std::logic_error("FAudio output device has already shut down");
	if (SharedDevice)
		return SharedDevice;

	SharedDevice = std::shared_ptr<FAudioOutputDevice>(
		new FAudioOutputDevice(requested));
	return SharedDevice;
}

void MusicPlayerLibrary::FAudioOutputDevice::ShutdownShared() noexcept
{
	std::shared_ptr<FAudioOutputDevice> released;
	{
		std::lock_guard lock(SharedDeviceMutex);
		SharedDeviceShutdown = true;
		released.swap(SharedDevice);
	}
	released.reset();
}

MusicPlayerLibrary::AudioOutputFormat
MusicPlayerLibrary::FAudioOutputDevice::ResolveSinkFormat(
	const AudioOutputFormat& requested) const
{
	return ResolveAudioOutputFormat(requested, system_format_);
}

std::uint32_t
MusicPlayerLibrary::FAudioOutputDevice::GetCurrentLatencyInSamples() const noexcept
{
	if (!engine_)
		return 0;
	FAudioPerformanceData performance{};
	FAudio_GetPerformanceData(engine_, &performance);
	return performance.CurrentLatencyInSamples;
}

void MusicPlayerLibrary::FAudioOutputDevice::SetMasterVolume(float volume) noexcept
{
	volume = std::clamp(volume, 0.0f, 1.0f);
	if (mastering_voice_)
		(void)FAudioVoice_SetVolume(mastering_voice_, volume, FAUDIO_COMMIT_NOW);
}

MusicPlayerLibrary::FAudioOutputDevice::~FAudioOutputDevice()
{
	if (mastering_voice_)
	{
		FAudioVoice_DestroyVoice(mastering_voice_);
		mastering_voice_ = nullptr;
	}
	if (engine_)
	{
		FAudio_Release(engine_);
		engine_ = nullptr;
	}
}

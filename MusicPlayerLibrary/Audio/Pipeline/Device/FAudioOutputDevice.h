// SPDX-License-Identifier: MIT

#pragma once

#include <memory>
#include <mutex>

#include <FAudio.h>

#include "Audio/AudioOutputFormat.h"

namespace MusicPlayerLibrary
{
	class FAudioOutputDevice final
	{
		FAudio* engine_ = nullptr;
		FAudioMasteringVoice* mastering_voice_ = nullptr;
		AudioOutputFormat output_format_{};
		FAudioWaveFormatExtensible system_format_{};

		explicit FAudioOutputDevice(const AudioOutputFormat& requested);

	public:
		FAudioOutputDevice(const FAudioOutputDevice&) = delete;
		FAudioOutputDevice& operator=(const FAudioOutputDevice&) = delete;
		~FAudioOutputDevice();

		[[nodiscard]] static std::shared_ptr<FAudioOutputDevice> Acquire(
			const AudioOutputFormat& requested = {});
		// Releases the application-lifetime cache. Existing sinks retain their
		// shared reference until their source voices have been destroyed.
		static void ShutdownShared() noexcept;
		[[nodiscard]] FAudio* GetEngine() const noexcept { return engine_; }
		[[nodiscard]] const AudioOutputFormat& GetOutputFormat() const noexcept
		{
			return output_format_;
		}
		[[nodiscard]] AudioOutputFormat ResolveSinkFormat(
			const AudioOutputFormat& requested) const;
		[[nodiscard]] std::uint32_t GetCurrentLatencyInSamples() const noexcept;
		void SetMasterVolume(float volume) noexcept;
	};
}

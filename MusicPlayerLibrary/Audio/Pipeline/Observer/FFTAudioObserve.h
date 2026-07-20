// SPDX-License-Identifier: MIT

#pragma once

#include <memory>
#include <mutex>

#include "Audio/Pipeline/AudioPipeline.h"

namespace MusicPlayerLibrary
{
	class FFTExecuter;

	class FFTAudioObserve final : public IAudioObserve
	{
		AudioOutputFormat format_{};
		std::unique_ptr<FFTExecuter> executer_;
		AudioStreamGeneration generation_ = 0;
		mutable std::mutex mutex_;

	public:
		explicit FFTAudioObserve(const AudioOutputFormat& format);
		~FFTAudioObserve() override;

		void OnFormat(
			const AudioOutputFormat& format,
			AudioStreamGeneration generation) override;
		void OnPcm(const NormalizedPcmBlock& block) override;
		void OnReset(AudioStreamGeneration generation) override;
		void OnEndOfStream(AudioStreamGeneration generation) override;
		int CopySpectrum(
			float* destination,
			int destination_length,
			AudioStreamGeneration generation,
			std::uint64_t media_frames_presented) const;
	};
}

// SPDX-License-Identifier: MIT

#pragma once
#include "Audio/Pipeline/Source/AudioFile.h"

namespace MusicPlayerLibrary {
	// Source compatibility for existing native and managed consumers. New code
	// should use AudioFile, which exposes the IAudioSource boundary explicitly.
	using MusicPlayer = AudioFile;
}

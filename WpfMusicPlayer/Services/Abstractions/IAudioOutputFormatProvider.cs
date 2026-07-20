// SPDX-License-Identifier: MIT

using WpfMusicPlayer.Models;

namespace WpfMusicPlayer.Services.Abstractions;

public interface IAudioOutputFormatProvider
{
    SystemAudioOutputFormat GetSystemDefaultOutputFormat();
}

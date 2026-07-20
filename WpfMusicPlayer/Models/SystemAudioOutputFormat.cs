// SPDX-License-Identifier: MIT

using static WpfMusicPlayer.Models.ConfigData;

namespace WpfMusicPlayer.Models;

public readonly record struct SystemAudioOutputFormat(
    int SampleRate,
    AudioSettings.ChannelType Channel,
    AudioSettings.BitDepthType BitDepth);

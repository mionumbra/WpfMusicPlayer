// SPDX-License-Identifier: MIT

using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;

namespace WpfMusicPlayer.ViewModels;

public class EqualizerPreset(string name, int[] values)
{
    public string Name { get; } = name;
    public int[] Values { get; } = values;
}

public class EqualizerViewModel : ObservableObject
{
    private static readonly int[] FrequenciesHz = [31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000];
    private readonly Action<int, int>? _applyBand;
    private bool _suppressPresetSwitch;
    private EqualizerPreset? _selectedPreset;

    public EqualizerViewModel(Action<int, int>? applyBand = null)
    {
        _applyBand = applyBand;

        string[] labels = ["31", "62", "125", "250", "500", "1k", "2k", "4k", "8k", "16k"];
        for (var i = 0; i < FrequenciesHz.Length; i++)
        {
            Bands.Add(new EqualizerBandViewModel(i, FrequenciesHz[i], labels[i], OnBandValueChanged));
        }

        // 欢迎提交PR提供更多预设值！
        // 随便调的，感觉还行
        Presets =
        [
            new("默认",     [0,  0,  0,  0,  0,  0,  0,  0,  0,  0]),
            new("完美低音", [6,  4, -5,  2,  3,  4,  4,  5,  5,  6]),
            new("极致摇滚", [6,  4,  0, -2, -6,  1,  4,  6,  7,  9]),
            new("最毒人声", [4,  0,  1,  2,  3,  4,  5,  4,  3,  3])
        ];

        SelectedPreset = Presets[0];
    }

    public ObservableCollection<EqualizerBandViewModel> Bands { get; } = [];

    public List<EqualizerPreset> Presets { get; }

    public EqualizerPreset? SelectedPreset
    {
        get => _selectedPreset;
        set
        {
            if (!SetProperty(ref _selectedPreset, value) || value == null || _suppressPresetSwitch) return;
            ApplyPreset(value);
        }
    }

    private void ApplyPreset(EqualizerPreset preset)
    {
        _suppressPresetSwitch = true;
        for (var i = 0; i < 10 && i < preset.Values.Length; i++)
        {
            if (!Bands[i].IsEnabled) continue;
            Bands[i].Value = preset.Values[i];
        }
        _suppressPresetSwitch = false;
    }

    private void OnBandValueChanged(int index, int value)
    {
        _applyBand?.Invoke(index, value);

        if (_suppressPresetSwitch) return;

        if (SelectedPreset != null && MatchesPreset(SelectedPreset)) return;

        SetSelectedPresetWithoutApplying(null);
    }

    private bool MatchesPreset(EqualizerPreset preset)
    {
        for (var i = 0; i < 10; i++)
        {
            if (!Bands[i].IsEnabled) continue;
            if (Bands[i].Value != preset.Values[i]) return false;
        }
        return true;
    }

    public void SetSampleRate(int sampleRate)
    {
        foreach (var band in Bands)
        {
            band.UpdateAvailability(sampleRate);
        }

        UpdateSelectedPresetFromCurrentValues();
    }

    public bool IsBandEnabled(int index) =>
        index >= 0 && index < Bands.Count && Bands[index].IsEnabled;

    // 从播放器获取当前均衡器设置并更新界面
    public void SyncFromPlayer(Func<int, int> getBand)
    {
        _suppressPresetSwitch = true;
        for (var i = 0; i < 10; i++)
        {
            if (!Bands[i].IsEnabled) continue;
            Bands[i].Value = Math.Clamp(getBand(i), -12, 12);
        }
        _suppressPresetSwitch = false;

        UpdateSelectedPresetFromCurrentValues();
    }

    private void UpdateSelectedPresetFromCurrentValues()
    {
        foreach (var preset in Presets)
        {
            if (!MatchesPreset(preset)) continue;
            SetSelectedPresetWithoutApplying(preset);
            return;
        }
        SetSelectedPresetWithoutApplying(null);
    }

    private void SetSelectedPresetWithoutApplying(EqualizerPreset? preset)
    {
        _suppressPresetSwitch = true;
        SelectedPreset = preset;
        _suppressPresetSwitch = false;
    }
}

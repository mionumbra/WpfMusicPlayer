// SPDX-License-Identifier: MIT

using CommunityToolkit.Mvvm.ComponentModel;

namespace WpfMusicPlayer.ViewModels;

public class EqualizerBandViewModel(int index, int frequencyHz, string label, Action<int, int> onValueChanged) : ObservableObject
{
    public string Label { get; } = label;

    public int FrequencyHz { get; } = frequencyHz;

    public bool IsEnabled
    {
        get;
        private set => SetProperty(ref field, value);
    } = true;

    public int Value
    {
        get;
        set
        {
            value = Math.Clamp(value, -12, 12);
            if (!SetProperty(ref field, value)) return;
            OnPropertyChanged(nameof(DisplayValue));
            if (IsEnabled)
            {
                onValueChanged(index, value);
            }
        }
    }

    public string DisplayValue => Value >= 0 ? $"+{Value}" : Value.ToString();

    public void UpdateAvailability(int sampleRate)
    {
        IsEnabled = sampleRate <= 0 || FrequencyHz * 2 < sampleRate;
    }
}

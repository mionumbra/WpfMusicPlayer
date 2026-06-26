// SPDX-License-Identifier: MIT

using System.Windows;
using System.Windows.Input;

namespace WpfMusicPlayer.Helpers;

public partial class OffsetAdjustDialog
{
    private const int Step = 10;

    private int _value;
    private int _originalValue;
    private int _minValue;
    private int _maxValue;
    private Action<int>? _onChanged;

    public int Result { get; private set; }

    private OffsetAdjustDialog()
    {
        InitializeComponent();
    }

    // 延迟调节，支持实时响应对话框内改变的延迟值
    // 传入onChanged事件即可
    public static int Show(int initialValue = 0, string title = "调整延迟",
        Action<int>? onChanged = null, int minValue = -10000, int maxValue = 10000)
    {
        var owner = Application.Current.Windows
            .OfType<Window>()
            .FirstOrDefault(w => w.IsActive)
            ?? Application.Current.MainWindow;

        var dlg = new OffsetAdjustDialog
        {
            _value = initialValue,
            _originalValue = initialValue,
            _minValue = minValue,
            _maxValue = maxValue,
            _onChanged = onChanged,
            TitleText =
            {
                Text = title
            },
            Result = initialValue
        };

        dlg.RefreshDisplay();

        if (owner != null)
            dlg.Owner = owner;

        dlg.ShowDialog();
        return dlg.Result;
    }

    private void RefreshDisplay()
    {
        ValueTextBox.Text = _value.ToString();
    }

    private void ApplyTextBoxValue()
    {
        if (int.TryParse(ValueTextBox.Text, out var parsed))
        {
            _value = Math.Clamp(parsed, _minValue, _maxValue);
        }
        else
        {
            _value = _originalValue;
        }
        RefreshDisplay();
    }

    private void NotifyChanged()
    {
        _onChanged?.Invoke(_value);
    }

    private void IncreaseButton_Click(object sender, RoutedEventArgs e)
    {
        ApplyTextBoxValue();
        _value = Math.Min(_value + Step, _maxValue);
        RefreshDisplay();
        NotifyChanged();
    }

    private void DecreaseButton_Click(object sender, RoutedEventArgs e)
    {
        ApplyTextBoxValue();
        _value = Math.Max(_value - Step, _minValue);
        RefreshDisplay();
        NotifyChanged();
    }

    private void QuickStep_Click(object sender, RoutedEventArgs e)
    {
        if (sender is not System.Windows.Controls.Button btn ||
            !int.TryParse(btn.Tag?.ToString(), out int delta)) return;
        ApplyTextBoxValue();
        _value = Math.Clamp(_value + delta, _minValue, _maxValue);
        RefreshDisplay();
        NotifyChanged();
    }

    private void ResetButton_Click(object sender, RoutedEventArgs e)
    {
        _value = _originalValue;
        RefreshDisplay();
        NotifyChanged();
    }

    private void OkButton_Click(object sender, RoutedEventArgs e)
    {
        ApplyTextBoxValue();
        Result = _value;
        DialogResult = true;
        Close();
    }

    private void ValueTextBox_LostFocus(object sender, RoutedEventArgs e)
    {
        ApplyTextBoxValue();
        NotifyChanged();
    }

    private void OffsetAdjustDialog_OnPreviewKeyDown(object sender, KeyEventArgs e)
    {
        switch (e.Key)
        {
            case Key.Up:
                _value = Math.Min(_value + Step, _maxValue);
                RefreshDisplay();
                NotifyChanged();
                e.Handled = true;
                break;
            case Key.Down:
                _value = Math.Max(_value - Step, _minValue);
                RefreshDisplay();
                NotifyChanged();
                e.Handled = true;
                break;
            case Key.Enter:
                e.Handled = true;
                ApplyTextBoxValue();
                Result = _value;
                DialogResult = true;
                Close();
                break;
        }
    }
}

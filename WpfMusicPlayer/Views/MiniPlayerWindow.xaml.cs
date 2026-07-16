// SPDX-License-Identifier: MIT

using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using WpfMusicPlayer.ViewModels;

namespace WpfMusicPlayer.Views;

public partial class MiniPlayerWindow : Window
{
    public event EventHandler? RestoreRequested;

    public MiniPlayerWindow(MainViewModel viewModel)
    {
        InitializeComponent();
        DataContext = viewModel;
    }

    private void RestoreButton_Click(object sender, RoutedEventArgs e)
    {
        RestoreRequested?.Invoke(this, EventArgs.Empty);
    }

    private void RootBorder_MouseLeftButtonDown(object sender, MouseButtonEventArgs e)
    {
        if (e.ButtonState != MouseButtonState.Pressed || IsWithinButton(e.OriginalSource as DependencyObject))
            return;

        DragMove();
    }

    private static bool IsWithinButton(DependencyObject? source)
    {
        while (source != null)
        {
            if (source is Button)
                return true;

            source = VisualTreeHelper.GetParent(source);
        }

        return false;
    }
}

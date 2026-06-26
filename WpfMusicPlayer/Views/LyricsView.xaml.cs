// SPDX-License-Identifier: MIT

using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using WpfMusicPlayer.ViewModels;

namespace WpfMusicPlayer.Views;

public partial class LyricsView : UserControl
{
    public static readonly DependencyProperty ButtonOrientationProperty =
        DependencyProperty.Register(nameof(ButtonOrientation), typeof(Orientation), typeof(LyricsView),
            new PropertyMetadata(Orientation.Horizontal));

    public Orientation ButtonOrientation
    {
        get => (Orientation)GetValue(ButtonOrientationProperty);
        set => SetValue(ButtonOrientationProperty, value);
    }

    public ListBox LyricsList => InternalLyricsList;
    public TranslateTransform LyricsTranslate => LyricsTranslateTransform;

    public LyricsView()
    {
        InitializeComponent();
    }

    private void LyricsList_PreviewMouseLeftButtonUp(object sender, MouseButtonEventArgs e)
    {
        if (sender is not ListBox) return;

        var source = e.OriginalSource as DependencyObject;
        while (source is not null and not ListBoxItem)
            source = VisualTreeHelper.GetParent(source);

        if (source is ListBoxItem { DataContext: LyricLineViewModel lyric })
        {
            if (DataContext is LyricsViewModel vm)
                vm.SeekToLyric(lyric);
        }
    }

    private void LyricsList_PreviewMouseWheel(object sender, MouseWheelEventArgs e)
    {
        if (sender is not ListBox listBox) return;

        var parentScrollViewer = FindParentScrollViewer(listBox);
        if (parentScrollViewer == null) return;

        parentScrollViewer.ScrollToVerticalOffset(parentScrollViewer.VerticalOffset - e.Delta);
        e.Handled = true;
    }

    private static ScrollViewer? FindParentScrollViewer(DependencyObject child)
    {
        var parent = VisualTreeHelper.GetParent(child);
        while (parent != null)
        {
            if (parent is ScrollViewer sv) return sv;
            parent = VisualTreeHelper.GetParent(parent);
        }
        return null;
    }
}

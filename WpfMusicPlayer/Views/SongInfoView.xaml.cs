// SPDX-License-Identifier: MIT

using System.Windows;
using System.Windows.Controls;

namespace WpfMusicPlayer.Views;

public partial class SongInfoView : UserControl
{
    public static readonly DependencyProperty TitleFontSizeProperty =
        DependencyProperty.Register(nameof(TitleFontSize), typeof(double), typeof(SongInfoView),
            new PropertyMetadata(18.0));

    public static readonly DependencyProperty TitleMaxHeightProperty =
        DependencyProperty.Register(nameof(TitleMaxHeight), typeof(double), typeof(SongInfoView),
            new PropertyMetadata(24.0));

    public static readonly DependencyProperty TitleBottomMarginProperty =
        DependencyProperty.Register(nameof(TitleBottomMargin), typeof(Thickness), typeof(SongInfoView),
            new PropertyMetadata(new Thickness(0, 0, 0, 2)));

    public static readonly DependencyProperty ArtistMaxHeightProperty =
        DependencyProperty.Register(nameof(ArtistMaxHeight), typeof(double), typeof(SongInfoView),
            new PropertyMetadata(18.0));

    public double TitleFontSize
    {
        get => (double)GetValue(TitleFontSizeProperty);
        set => SetValue(TitleFontSizeProperty, value);
    }

    public double TitleMaxHeight
    {
        get => (double)GetValue(TitleMaxHeightProperty);
        set => SetValue(TitleMaxHeightProperty, value);
    }

    public Thickness TitleBottomMargin
    {
        get => (Thickness)GetValue(TitleBottomMarginProperty);
        set => SetValue(TitleBottomMarginProperty, value);
    }

    public double ArtistMaxHeight
    {
        get => (double)GetValue(ArtistMaxHeightProperty);
        set => SetValue(ArtistMaxHeightProperty, value);
    }

    public SongInfoView()
    {
        InitializeComponent();
    }
}

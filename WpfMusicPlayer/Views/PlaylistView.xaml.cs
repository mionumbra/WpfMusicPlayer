// SPDX-License-Identifier: MIT

using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using WpfMusicPlayer.ViewModels;

namespace WpfMusicPlayer.Views;

public partial class PlaylistView : UserControl
{
    public PlaylistView()
    {
        InitializeComponent();
    }

    private void PlaylistView_PreviewMouseLeftButtonDown(object sender, MouseButtonEventArgs e)
    {
        if (PlaylistTitleEdit.Visibility != Visibility.Visible) return;

        var hit = e.OriginalSource as DependencyObject;
        while (hit != null)
        {
            if (ReferenceEquals(hit, PlaylistTitleEdit)) return;
            hit = VisualTreeHelper.GetParent(hit);
        }

        FinishTitleEditing();
    }

    private void PlaylistListBox_PreviewMouseLeftButtonUp(object sender, MouseButtonEventArgs e)
    {
        if (sender is not ListBox) return;

        var source = e.OriginalSource as DependencyObject;
        while (source is not null and not ListBoxItem)
            source = VisualTreeHelper.GetParent(source);

        if (source is ListBoxItem { DataContext: PlaylistItemViewModel item })
        {
            if (DataContext is PlaylistViewModel vm)
                vm.PlayFromPlaylist(item);
        }
    }
    
    private void PlaylistListBox_OnPreviewMouseWheel(object sender, MouseWheelEventArgs e)
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

    private void PlaylistTitleBlock_MouseLeftButtonUp(object sender, MouseButtonEventArgs e)
    {
        PlaylistTitleBlock.Visibility = Visibility.Collapsed;
        PlaylistTitleEdit.Visibility = Visibility.Visible;
        PlaylistTitleEdit.Focus();
        PlaylistTitleEdit.SelectAll();
    }

    private void PlaylistTitleEdit_LostFocus(object sender, RoutedEventArgs e)
    {
        FinishTitleEditing();
    }

    private void PlaylistTitleEdit_KeyDown(object sender, KeyEventArgs e)
    {
        if (e.Key == Key.Enter || e.Key == Key.Escape)
        {
            FinishTitleEditing();
            e.Handled = true;
        }
    }

    private void FinishTitleEditing()
    {
        PlaylistTitleEdit.Visibility = Visibility.Collapsed;
        PlaylistTitleBlock.Visibility = Visibility.Visible;
    }

    private void CoverBorder_MouseLeftButtonUp(object sender, MouseButtonEventArgs e)
    {
        if (DataContext is PlaylistViewModel vm)
        {
            vm.ChangePlaylistCoverCommand.Execute(null);
        }
    }
}

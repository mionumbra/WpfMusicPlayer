// SPDX-License-Identifier: MIT

using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using WpfMusicPlayer.ViewModels;

namespace WpfMusicPlayer.Views;

public partial class PlayerToolbarView : UserControl
{
    public SongInfoView LandscapeSongInfo => InternalLandscapeSongInfo;
    public TranslateTransform LandscapeSongInfoTranslate => (TranslateTransform)InternalLandscapeSongInfo.RenderTransform;
    public StackPanel VolumePanelElement => InternalVolumePanel;

    public PlayerToolbarView()
    {
        InitializeComponent(); 
    }

    private void ProgressSlider_PreviewMouseDown(object sender, MouseButtonEventArgs e)
    {
        if (DataContext is MainViewModel vm)
            vm.IsDraggingSlider = true;
    }

    private void ProgressSlider_PreviewMouseUp(object sender, MouseButtonEventArgs e) {
        if(DataContext is MainViewModel vm)
            {
            if(sender is Slider slider) {
                var total = slider.ActualWidth;
                var curr = e.GetPosition(slider).X;
                var pct = (float)(curr / total);
                vm.SeekToCurrentPosition(pct, (float)(1 / total * 2));
            }
        }
    }
    private void ProgressSlider_ValueChanged(object sender, System.Windows.RoutedPropertyChangedEventArgs<double> e) {
        //if(DataContext is MainViewModel vm) {
        //    if(vm.IsDraggingSlider) {
        //        return;
        //    }
        //    if(sender is Slider slider) {
        //        vm.IsDraggingSlider = true;
        //        var total = slider.ActualWidth;
        //        var curr = e.NewValue;
        //        var pct = (float)(curr / total);
        //       // vm.SeekToCurrentPosition(pct);
        //        vm.IsDraggingSlider = false;
        //    }
        //}
    }
}

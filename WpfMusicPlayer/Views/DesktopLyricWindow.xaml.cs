// SPDX-License-Identifier: MIT

using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Media.Animation;
using System.Windows.Threading;
using WpfMusicPlayer.ViewModels;

namespace WpfMusicPlayer.Views;

public partial class DesktopLyricWindow : Window
{
    private readonly DesktopLyricViewModel _viewModel;
    private bool _isLocked;
    private bool _isPassthrough;
    private bool _unlockBarShowing;
    private DispatcherTimer? _hoverTimer;
    private DispatcherTimer? _hideTimer;

    private const int GWL_EXSTYLE = -20;
    private const int WS_EX_TRANSPARENT = 0x00000020;
    private const int WS_EX_TOOLWINDOW = 0x00000080;

    // 控制条区域高度（鼠标进入此区域时临时移除穿透，允许点击解锁按钮）
    private const double ControlBarHeight = 36;

    [LibraryImport("user32.dll")]
    private static partial int GetWindowLongPtrW(IntPtr hwnd, int index);

    [LibraryImport("user32.dll")]
    private static partial int SetWindowLongPtrW(IntPtr hwnd, int index, int newStyle);

    [LibraryImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static partial bool GetCursorPos(out POINT lpPoint);

    [StructLayout(LayoutKind.Sequential)]
    private struct POINT { public int X, Y; }

    public DesktopLyricWindow(DesktopLyricViewModel viewModel)
    {
        InitializeComponent();
        _viewModel = viewModel;
        DataContext = viewModel;

        SourceInitialized += (_, _) =>
        {
            var hwnd = new WindowInteropHelper(this).Handle;
            var exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
            SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle | WS_EX_TOOLWINDOW);
        };

        Loaded += (_, _) =>
        {
            var workArea = SystemParameters.WorkArea;
            Left = (workArea.Width - Width) / 2;
            Top = workArea.Bottom - Height - 80;
        };
    }

    private void Window_MouseLeftButtonDown(object sender, MouseButtonEventArgs e)
    {
        if (!_isLocked)
            DragMove();
    }

    private void RootGrid_MouseEnter(object sender, MouseEventArgs e)
    {
        if (_isLocked) return; // 锁定时由定时器控制
        // 300ms内鼠标重进入hit test区域，取消退出并恢复可见性
        _hideTimer?.Stop();
        AnimateOpacity(ControlBar, 1.0, 200);
        AnimateOpacity(HoverBackground, 1.0, 200);
    }

    private void RootGrid_MouseLeave(object sender, MouseEventArgs e)
    {
        if (_isLocked) return; // 锁定时由定时器控制
        // 延迟300ms，避免布局不稳定时立刻退出hit test区域导致点不了按钮
        _hideTimer ??= new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(300) };
        _hideTimer.Tick -= OnHideTimerTick;
        _hideTimer.Tick += OnHideTimerTick;
        _hideTimer.Stop();
        _hideTimer.Start();
    }

    private void OnHideTimerTick(object? sender, EventArgs e)
    {
        // 300ms已到，如果没有执行操作则执行淡出
        _hideTimer!.Stop();
        AnimateOpacity(ControlBar, 0.0, 200);
        AnimateOpacity(HoverBackground, 0.0, 200);
    }

    private void IncreaseFont_Click(object sender, RoutedEventArgs e)
        => _viewModel.FontSize = Math.Min(_viewModel.FontSize + 2, 40);

    private void DecreaseFont_Click(object sender, RoutedEventArgs e)
        => _viewModel.FontSize = Math.Max(_viewModel.FontSize - 2, 14);

    private void Lock_Click(object sender, RoutedEventArgs e)
    {
        _isLocked = true;
        LockIcon.Text = "\uE72E";
        ControlBar.Visibility = Visibility.Collapsed;
        AnimateOpacity(HoverBackground, 0.0, 200);
        SetPassthrough(true);
        StartHoverDetection();
    }

    public void Unlock()
    {
        _isLocked = false;
        _unlockBarShowing = false;
        LockIcon.Text = "\uE785";
        ControlBar.Visibility = Visibility.Visible;
        SetPassthrough(false);
        StopHoverDetection();
    }

    private void Unlock_Click(object sender, RoutedEventArgs e)
    {
        Unlock();
        AnimateOpacity(UnlockBar, 0.0, 200);
        AnimateOpacity(ControlBar, 1.0, 200);
    }

    public bool IsLocked => _isLocked;

    private void Close_Click(object sender, RoutedEventArgs e)
    {
        _viewModel.IsDesktopLyricVisible = false;
    }

    // 设置WS_EX_TRANSPARENT，让事件穿透整个窗体
    private void SetPassthrough(bool enable)
    {
        if (enable == _isPassthrough) return;
        _isPassthrough = enable;

        var hwnd = new WindowInteropHelper(this).Handle;
        var exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, enable
            ? exStyle | WS_EX_TRANSPARENT
            : exStyle & ~WS_EX_TRANSPARENT);
    }

    // WS_EX_TRANSPARENT会过滤所有鼠标事件，因此需要定时器手动轮询
    private void StartHoverDetection()
    {
        if (_hoverTimer is { IsEnabled: true }) return;
        _hoverTimer ??= new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(50) };
        _hoverTimer.Tick += CheckHover;
        _hoverTimer.Start();
    }

    private void StopHoverDetection()
    {
        if (_hoverTimer == null) return;
        _hoverTimer.Tick -= CheckHover;
        _hoverTimer.Stop();
    }

    // 用定时器是下下策，因为WM_NCHITTEST在WPF AllowsTransparency分层窗口下不可靠
    private void CheckHover(object? sender, EventArgs e)
    {
        GetCursorPos(out var pt);
        var windowPoint = PointFromScreen(new Point(pt.X, pt.Y));
        var isInWindow = windowPoint.X >= 0 && windowPoint.Y >= 0 &&
                         windowPoint.X <= ActualWidth && windowPoint.Y <= ActualHeight;

        if (isInWindow)
        {
            if (!_unlockBarShowing)
            {
                _unlockBarShowing = true;
                AnimateOpacity(UnlockBar, 1.0, 200);
                AnimateOpacity(HoverBackground, 1.0, 200);
            }

            var isInControlBar = windowPoint.Y <= ControlBarHeight;
            // 控制条区域，移除穿透（解锁按钮可点击）
            // 歌词区域，保持穿透（点击穿透到桌面）
            SetPassthrough(!isInControlBar);
        }
        else if (_unlockBarShowing)
        {
            _unlockBarShowing = false;
            AnimateOpacity(UnlockBar, 0.0, 200);
            AnimateOpacity(HoverBackground, 0.0, 200);
            SetPassthrough(true);
        }
    }

    // 立方缓动，切换组件透明度；淡入前设为 Visible，淡出完成后设为 Collapsed
    private static void AnimateOpacity(UIElement element, double to, int durationMs)
    {
        if (to > 0)
            element.Visibility = Visibility.Visible;

        var animation = new DoubleAnimation(to, TimeSpan.FromMilliseconds(durationMs))
        {
            EasingFunction = new CubicEase { EasingMode = EasingMode.EaseOut }
        };

        if (to == 0)
            animation.Completed += (_, _) => element.Visibility = Visibility.Collapsed;

        element.BeginAnimation(UIElement.OpacityProperty, animation);
    }
}

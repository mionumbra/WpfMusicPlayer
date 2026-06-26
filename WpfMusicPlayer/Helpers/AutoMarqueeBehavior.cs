// SPDX-License-Identifier: MIT

using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Threading;

namespace WpfMusicPlayer.Helpers;

// Overflow-only horizontal marquee for compact one-line text.
public static class AutoMarqueeBehavior
{
    public static readonly DependencyProperty IsEnabledProperty =
        DependencyProperty.RegisterAttached(
            "IsEnabled", typeof(bool), typeof(AutoMarqueeBehavior),
            new PropertyMetadata(false, OnIsEnabledChanged));

    public static bool GetIsEnabled(DependencyObject obj) => (bool)obj.GetValue(IsEnabledProperty);
    public static void SetIsEnabled(DependencyObject obj, bool value) => obj.SetValue(IsEnabledProperty, value);

    public static readonly DependencyProperty SpeedProperty =
        DependencyProperty.RegisterAttached(
            "Speed", typeof(double), typeof(AutoMarqueeBehavior),
            new PropertyMetadata(28.0, OnLayoutPropertyChanged));

    public static double GetSpeed(DependencyObject obj) => (double)obj.GetValue(SpeedProperty);
    public static void SetSpeed(DependencyObject obj, double value) => obj.SetValue(SpeedProperty, value);

    public static readonly DependencyProperty PauseMillisecondsProperty =
        DependencyProperty.RegisterAttached(
            "PauseMilliseconds", typeof(int), typeof(AutoMarqueeBehavior),
            new PropertyMetadata(1200, OnLayoutPropertyChanged));

    public static int GetPauseMilliseconds(DependencyObject obj) => (int)obj.GetValue(PauseMillisecondsProperty);
    public static void SetPauseMilliseconds(DependencyObject obj, int value) => obj.SetValue(PauseMillisecondsProperty, value);

    private static void OnIsEnabledChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is not FrameworkElement element) return;

        if ((bool)e.NewValue)
        {
            element.Loaded += OnElementLayoutChanged;
            element.SizeChanged += OnElementLayoutChanged;

            if (element is System.Windows.Controls.TextBlock textBlock)
                textBlock.TargetUpdated += OnTextTargetUpdated;

            element.Dispatcher.BeginInvoke(() => UpdateAnimation(element), DispatcherPriority.Loaded);
        }
        else
        {
            element.Loaded -= OnElementLayoutChanged;
            element.SizeChanged -= OnElementLayoutChanged;

            if (element is System.Windows.Controls.TextBlock textBlock)
                textBlock.TargetUpdated -= OnTextTargetUpdated;

            ResetTransform(element);
        }
    }

    private static void OnLayoutPropertyChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is FrameworkElement element && GetIsEnabled(element))
            element.Dispatcher.BeginInvoke(() => UpdateAnimation(element), DispatcherPriority.Loaded);
    }

    private static void OnElementLayoutChanged(object sender, EventArgs e)
    {
        if (sender is FrameworkElement element && GetIsEnabled(element))
            element.Dispatcher.BeginInvoke(() => UpdateAnimation(element), DispatcherPriority.Loaded);
    }

    private static void OnTextTargetUpdated(object? sender, System.Windows.Data.DataTransferEventArgs e)
    {
        if (sender is FrameworkElement element && GetIsEnabled(element))
            element.Dispatcher.BeginInvoke(() => UpdateAnimation(element), DispatcherPriority.Loaded);
    }

    private static void UpdateAnimation(FrameworkElement element)
    {
        if (!GetIsEnabled(element) || VisualTreeHelper.GetParent(element) is not FrameworkElement parent)
        {
            ResetTransform(element);
            return;
        }

        element.Measure(new Size(double.PositiveInfinity, parent.ActualHeight));
        var contentWidth = Math.Max(element.DesiredSize.Width, element.ActualWidth);
        var containerWidth = parent.ActualWidth;
        var overflow = contentWidth - containerWidth;

        if (containerWidth <= 0 || overflow <= 1)
        {
            ResetTransform(element);
            return;
        }

        var speed = Math.Max(1.0, GetSpeed(element));
        var travelMs = Math.Max(600, overflow / speed * 1000);
        var pauseMs = Math.Max(0, GetPauseMilliseconds(element));

        var transform = EnsureTransform(element);
        var animation = new DoubleAnimationUsingKeyFrames
        {
            RepeatBehavior = RepeatBehavior.Forever
        };
        animation.KeyFrames.Add(new DiscreteDoubleKeyFrame(0, KeyTime.FromTimeSpan(TimeSpan.Zero)));
        animation.KeyFrames.Add(new LinearDoubleKeyFrame(0, KeyTime.FromTimeSpan(TimeSpan.FromMilliseconds(pauseMs))));
        animation.KeyFrames.Add(new EasingDoubleKeyFrame(-overflow, KeyTime.FromTimeSpan(TimeSpan.FromMilliseconds(pauseMs + travelMs)))
        {
            EasingFunction = new SineEase { EasingMode = EasingMode.EaseInOut }
        });
        animation.KeyFrames.Add(new LinearDoubleKeyFrame(-overflow, KeyTime.FromTimeSpan(TimeSpan.FromMilliseconds(pauseMs * 2 + travelMs))));

        transform.BeginAnimation(TranslateTransform.XProperty, animation);
    }

    private static TranslateTransform EnsureTransform(FrameworkElement element)
    {
        if (element.RenderTransform is TranslateTransform t)
        {
            t.BeginAnimation(TranslateTransform.XProperty, null);
            t.X = 0;
            return t;
        }

        var transform = new TranslateTransform();
        element.RenderTransform = transform;
        return transform;
    }

    private static void ResetTransform(FrameworkElement element)
    {
        if (element.RenderTransform is not TranslateTransform t) return;
        t.BeginAnimation(TranslateTransform.XProperty, null);
        t.X = 0;
    }
}

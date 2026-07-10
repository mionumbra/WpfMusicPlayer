// SPDX-License-Identifier: MIT

using System.ComponentModel;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Threading;

namespace WpfMusicPlayer.Helpers;

// Constant-speed marquee for a horizontal track whose first child is the primary content.
public static class AutoMarqueeBehavior
{
    private const double MinimumSpeed = 0.1;
    private const double OverflowTolerance = 1.0;

    private static readonly DependencyPropertyDescriptor TextPropertyDescriptor =
        DependencyPropertyDescriptor.FromProperty(TextBlock.TextProperty, typeof(TextBlock))!;

    public static readonly DependencyProperty IsEnabledProperty =
        DependencyProperty.RegisterAttached(
            "IsEnabled", typeof(bool), typeof(AutoMarqueeBehavior),
            new PropertyMetadata(false, OnIsEnabledChanged));

    public static bool GetIsEnabled(DependencyObject obj) => (bool)obj.GetValue(IsEnabledProperty);
    public static void SetIsEnabled(DependencyObject obj, bool value) => obj.SetValue(IsEnabledProperty, value);

    // Device-independent pixels per second.
    public static readonly DependencyProperty SpeedProperty =
        DependencyProperty.RegisterAttached(
            "Speed", typeof(double), typeof(AutoMarqueeBehavior),
            new PropertyMetadata(28.0, OnAnimationPropertyChanged), IsValidSpeed);

    public static double GetSpeed(DependencyObject obj) => (double)obj.GetValue(SpeedProperty);
    public static void SetSpeed(DependencyObject obj, double value) => obj.SetValue(SpeedProperty, value);

    // Blank distance between the primary content and its repeated copy.
    public static readonly DependencyProperty GapProperty =
        DependencyProperty.RegisterAttached(
            "Gap", typeof(double), typeof(AutoMarqueeBehavior),
            new PropertyMetadata(40.0, OnAnimationPropertyChanged), IsNonNegativeFiniteDouble);

    public static double GetGap(DependencyObject obj) => (double)obj.GetValue(GapProperty);
    public static void SetGap(DependencyObject obj, double value) => obj.SetValue(GapProperty, value);

    // Hold time at the fully aligned start of every loop.
    public static readonly DependencyProperty PauseMillisecondsProperty =
        DependencyProperty.RegisterAttached(
            "PauseMilliseconds", typeof(int), typeof(AutoMarqueeBehavior),
            new PropertyMetadata(2000, OnAnimationPropertyChanged), IsNonNegativeInt);

    public static int GetPauseMilliseconds(DependencyObject obj) =>
        (int)obj.GetValue(PauseMillisecondsProperty);

    public static void SetPauseMilliseconds(DependencyObject obj, int value) =>
        obj.SetValue(PauseMillisecondsProperty, value);

    private static readonly DependencyPropertyKey IsOverflowingPropertyKey =
        DependencyProperty.RegisterAttachedReadOnly(
            "IsOverflowing", typeof(bool), typeof(AutoMarqueeBehavior),
            new PropertyMetadata(false));

    public static readonly DependencyProperty IsOverflowingProperty =
        IsOverflowingPropertyKey.DependencyProperty;

    public static bool GetIsOverflowing(DependencyObject obj) =>
        (bool)obj.GetValue(IsOverflowingProperty);

    private static readonly DependencyProperty StateProperty =
        DependencyProperty.RegisterAttached(
            "State", typeof(MarqueeState), typeof(AutoMarqueeBehavior));

    private static readonly DependencyProperty PrimaryStateProperty =
        DependencyProperty.RegisterAttached(
            "PrimaryState", typeof(MarqueeState), typeof(AutoMarqueeBehavior));

    private static bool IsValidSpeed(object value) =>
        value is double number && double.IsFinite(number) && number >= MinimumSpeed;

    private static bool IsNonNegativeFiniteDouble(object value) =>
        value is double number && double.IsFinite(number) && number >= 0;

    private static bool IsNonNegativeInt(object value) =>
        value is int number && number >= 0;

    private static void OnIsEnabledChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is not FrameworkElement element) return;

        if ((bool)e.NewValue)
        {
            element.Loaded += OnElementLoaded;
            element.Unloaded += OnElementUnloaded;
            element.SizeChanged += OnElementSizeChanged;
            element.IsVisibleChanged += OnElementIsVisibleChanged;

            if (element.IsLoaded)
                QueueUpdate(element);
        }
        else
        {
            element.Loaded -= OnElementLoaded;
            element.Unloaded -= OnElementUnloaded;
            element.SizeChanged -= OnElementSizeChanged;
            element.IsVisibleChanged -= OnElementIsVisibleChanged;

            var state = GetState(element);
            if (state is not null)
            {
                CancelPendingUpdate(state);
                DetachFromParent(state);
                DetachFromPrimaryContent(state);
                element.ClearValue(StateProperty);
            }

            SetIsOverflowing(element, false);
            ResetTransform(element);
        }
    }

    private static void OnAnimationPropertyChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is FrameworkElement element && GetIsEnabled(element))
            QueueUpdate(element);
    }

    private static void OnElementLoaded(object sender, RoutedEventArgs e)
    {
        if (sender is not FrameworkElement element || !GetIsEnabled(element)) return;

        GetOrCreateState(element).Invalidate();
        QueueUpdate(element);
    }

    private static void OnElementUnloaded(object sender, RoutedEventArgs e)
    {
        if (sender is not FrameworkElement element) return;

        Suspend(element);
    }

    private static void OnElementSizeChanged(object sender, SizeChangedEventArgs e)
    {
        if (sender is FrameworkElement element && GetIsEnabled(element))
            QueueUpdate(element);
    }

    private static void OnElementIsVisibleChanged(object sender, DependencyPropertyChangedEventArgs e)
    {
        if (sender is not FrameworkElement element || !GetIsEnabled(element)) return;

        if (element.IsVisible)
        {
            GetOrCreateState(element).Invalidate();
            QueueUpdate(element);
        }
        else
        {
            Suspend(element);
        }
    }

    private static void OnPrimaryContentChanged(object? sender, EventArgs e)
    {
        if (sender is not FrameworkElement primaryContent) return;

        var state = GetPrimaryState(primaryContent);
        if (state?.Target is { } target && GetIsEnabled(target))
            QueueUpdate(target);
    }

    private static void OnPrimaryContentSizeChanged(object sender, SizeChangedEventArgs e)
    {
        OnPrimaryContentChanged(sender, e);
    }

    private static void QueueUpdate(FrameworkElement element)
    {
        if (!GetIsEnabled(element) || !element.IsLoaded || !element.IsVisible ||
            element.Dispatcher.HasShutdownStarted)
            return;

        var state = GetOrCreateState(element);
        if (state.PendingUpdate?.Status == DispatcherOperationStatus.Pending)
            return;

        state.PendingUpdate = element.Dispatcher.BeginInvoke(DispatcherPriority.Loaded, new Action(() =>
        {
            state.PendingUpdate = null;
            if (!GetIsEnabled(element) || !element.IsLoaded || !element.IsVisible) return;

            AttachToParent(element, state);
            AttachToPrimaryContent(element, state);
            UpdateAnimation(element, state);
        }));
    }

    private static void AttachToParent(FrameworkElement element, MarqueeState state)
    {
        var parent = VisualTreeHelper.GetParent(element) as FrameworkElement;
        if (ReferenceEquals(parent, state.Parent)) return;

        DetachFromParent(state);
        state.Parent = parent;

        if (parent is not null)
            parent.SizeChanged += state.ParentSizeChangedHandler;
    }

    private static void DetachFromParent(MarqueeState state)
    {
        if (state.Parent is not null)
            state.Parent.SizeChanged -= state.ParentSizeChangedHandler;

        state.Parent = null;
    }

    private static void AttachToPrimaryContent(FrameworkElement element, MarqueeState state)
    {
        var primaryContent = FindPrimaryContent(element);
        if (ReferenceEquals(primaryContent, state.PrimaryContent)) return;

        DetachFromPrimaryContent(state);
        state.PrimaryContent = primaryContent;

        if (primaryContent is null) return;

        primaryContent.SetValue(PrimaryStateProperty, state);
        primaryContent.SizeChanged += OnPrimaryContentSizeChanged;

        if (primaryContent is TextBlock textBlock)
            TextPropertyDescriptor.AddValueChanged(textBlock, OnPrimaryContentChanged);
    }

    private static void DetachFromPrimaryContent(MarqueeState state)
    {
        if (state.PrimaryContent is not { } primaryContent) return;

        primaryContent.SizeChanged -= OnPrimaryContentSizeChanged;

        if (primaryContent is TextBlock textBlock)
            TextPropertyDescriptor.RemoveValueChanged(textBlock, OnPrimaryContentChanged);

        if (ReferenceEquals(GetPrimaryState(primaryContent), state))
            primaryContent.ClearValue(PrimaryStateProperty);

        state.PrimaryContent = null;
    }

    private static FrameworkElement? FindPrimaryContent(FrameworkElement element)
    {
        if (element is not Panel panel) return element;

        foreach (UIElement child in panel.Children)
        {
            if (child is FrameworkElement frameworkElement)
                return frameworkElement;
        }

        return null;
    }

    private static void Suspend(FrameworkElement element)
    {
        var state = GetState(element);
        if (state is not null)
        {
            CancelPendingUpdate(state);
            DetachFromParent(state);
            DetachFromPrimaryContent(state);
            state.Invalidate();
        }

        SetIsOverflowing(element, false);
        ResetTransform(element);
    }

    private static void CancelPendingUpdate(MarqueeState state)
    {
        if (state.PendingUpdate?.Status == DispatcherOperationStatus.Pending)
            state.PendingUpdate.Abort();

        state.PendingUpdate = null;
    }

    private static void UpdateAnimation(FrameworkElement element, MarqueeState state)
    {
        var parent = state.Parent;
        var primaryContent = state.PrimaryContent;
        if (parent is null || primaryContent is null)
        {
            SetIsOverflowing(element, false);
            ResetTransform(element);
            state.Invalidate();
            return;
        }

        var measureHeight = parent.ActualHeight > 0
            ? parent.ActualHeight
            : double.PositiveInfinity;
        primaryContent.Measure(new Size(double.PositiveInfinity, measureHeight));

        var contentWidth = primaryContent.DesiredSize.Width;
        var containerWidth = parent.ActualWidth;
        var speed = GetSpeed(element);
        var gap = GetGap(element);
        var pauseMilliseconds = GetPauseMilliseconds(element);
        var contentKey = primaryContent is TextBlock textBlock ? textBlock.Text : null;
        var shouldAnimate = containerWidth > 0 && contentWidth - containerWidth > OverflowTolerance;
        var cycleDistance = contentWidth + gap;
        var pauseTime = default(TimeSpan);
        var endTime = default(TimeSpan);
        var canAnimate = shouldAnimate &&
                         TryCreateTimeline(
                             cycleDistance,
                             speed,
                             pauseMilliseconds,
                             out pauseTime,
                             out endTime);

        SetIsOverflowing(element, canAnimate);

        if (state.Matches(
                contentWidth,
                containerWidth,
                speed,
                gap,
                pauseMilliseconds,
                contentKey,
                canAnimate))
            return;

        if (!canAnimate)
        {
            ResetTransform(element);
            state.Record(
                contentWidth,
                containerWidth,
                speed,
                gap,
                pauseMilliseconds,
                contentKey,
                false);
            return;
        }

        var animation = new DoubleAnimationUsingKeyFrames
        {
            RepeatBehavior = RepeatBehavior.Forever
        };
        animation.KeyFrames.Add(
            new DiscreteDoubleKeyFrame(0, KeyTime.FromTimeSpan(TimeSpan.Zero)));

        if (pauseTime > TimeSpan.Zero)
        {
            animation.KeyFrames.Add(
                new LinearDoubleKeyFrame(0, KeyTime.FromTimeSpan(pauseTime)));
        }

        animation.KeyFrames.Add(
            new LinearDoubleKeyFrame(-cycleDistance, KeyTime.FromTimeSpan(endTime)));

        var transform = EnsureTransform(element);
        transform.BeginAnimation(
            TranslateTransform.XProperty,
            animation,
            HandoffBehavior.SnapshotAndReplace);

        state.Record(
            contentWidth,
            containerWidth,
            speed,
            gap,
            pauseMilliseconds,
            contentKey,
            true);
    }

    private static bool TryCreateTimeline(
        double cycleDistance,
        double speed,
        int pauseMilliseconds,
        out TimeSpan pauseTime,
        out TimeSpan endTime)
    {
        pauseTime = TimeSpan.FromMilliseconds(pauseMilliseconds);
        endTime = default;

        var travelSeconds = cycleDistance / speed;
        if (!double.IsFinite(travelSeconds) || travelSeconds <= 0)
            return false;

        try
        {
            var travelTime = TimeSpan.FromSeconds(travelSeconds);
            if (travelTime <= TimeSpan.Zero || travelTime > TimeSpan.MaxValue - pauseTime)
                return false;

            endTime = pauseTime + travelTime;
            return true;
        }
        catch (OverflowException)
        {
            return false;
        }
    }

    private static TranslateTransform EnsureTransform(FrameworkElement element)
    {
        if (element.RenderTransform is TranslateTransform transform)
        {
            transform.BeginAnimation(TranslateTransform.XProperty, null);
            transform.X = 0;
            return transform;
        }

        var newTransform = new TranslateTransform();
        element.RenderTransform = newTransform;
        return newTransform;
    }

    private static void ResetTransform(FrameworkElement element)
    {
        if (element.RenderTransform is not TranslateTransform transform) return;

        transform.BeginAnimation(TranslateTransform.XProperty, null);
        transform.X = 0;
    }

    private static void SetIsOverflowing(DependencyObject element, bool value)
    {
        if (GetIsOverflowing(element) != value)
            element.SetValue(IsOverflowingPropertyKey, value);
    }

    private static MarqueeState? GetState(DependencyObject element) =>
        (MarqueeState?)element.GetValue(StateProperty);

    private static MarqueeState? GetPrimaryState(DependencyObject element) =>
        (MarqueeState?)element.GetValue(PrimaryStateProperty);

    private static MarqueeState GetOrCreateState(FrameworkElement element)
    {
        var state = GetState(element);
        if (state is not null) return state;

        state = new MarqueeState(element);
        element.SetValue(StateProperty, state);
        return state;
    }

    private sealed class MarqueeState
    {
        private const double SizeTolerance = 0.1;

        private double _contentWidth = double.NaN;
        private double _containerWidth = double.NaN;
        private double _speed = double.NaN;
        private double _gap = double.NaN;
        private int _pauseMilliseconds = -1;
        private string? _contentKey;
        private bool _isAnimating;

        public MarqueeState(FrameworkElement target)
        {
            Target = target;
            ParentSizeChangedHandler = (_, _) => QueueUpdate(target);
        }

        public FrameworkElement Target { get; }
        public FrameworkElement? Parent { get; set; }
        public FrameworkElement? PrimaryContent { get; set; }
        public SizeChangedEventHandler ParentSizeChangedHandler { get; }
        public DispatcherOperation? PendingUpdate { get; set; }

        public bool Matches(
            double contentWidth,
            double containerWidth,
            double speed,
            double gap,
            int pauseMilliseconds,
            string? contentKey,
            bool isAnimating) =>
            Math.Abs(_contentWidth - contentWidth) < SizeTolerance &&
            Math.Abs(_containerWidth - containerWidth) < SizeTolerance &&
            _speed.Equals(speed) &&
            _gap.Equals(gap) &&
            _pauseMilliseconds == pauseMilliseconds &&
            string.Equals(_contentKey, contentKey, StringComparison.Ordinal) &&
            _isAnimating == isAnimating;

        public void Record(
            double contentWidth,
            double containerWidth,
            double speed,
            double gap,
            int pauseMilliseconds,
            string? contentKey,
            bool isAnimating)
        {
            _contentWidth = contentWidth;
            _containerWidth = containerWidth;
            _speed = speed;
            _gap = gap;
            _pauseMilliseconds = pauseMilliseconds;
            _contentKey = contentKey;
            _isAnimating = isAnimating;
        }

        public void Invalidate()
        {
            _contentWidth = double.NaN;
            _containerWidth = double.NaN;
            _speed = double.NaN;
            _gap = double.NaN;
            _pauseMilliseconds = -1;
            _contentKey = null;
            _isAnimating = false;
        }
    }
}

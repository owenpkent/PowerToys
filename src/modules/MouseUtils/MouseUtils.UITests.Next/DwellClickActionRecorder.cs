// Copyright (c) Microsoft Corporation
// The Microsoft Corporation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

using System.Collections.Concurrent;
using System.Drawing;
using System.Runtime.InteropServices;
using Forms = System.Windows.Forms;

namespace MouseUtils.UITests;

/// <summary>
/// A plain window that records every mouse button, wheel, drag-move, and key message it receives,
/// with screen coordinates. The Dwell Click action tests rest the pointer on it and assert on
/// exactly what arrived, so they check what an application actually sees rather than only the
/// global button state. It also records every keystroke Dwell Click injects (by the module's
/// injection tag), which is how the keystroke buttons are checked: the shell surfaces they open
/// (Start, Task view, the notification center) do not reliably take the foreground in time to be
/// observed, but the keys the module sends are exact.
/// </summary>
internal sealed class DwellClickActionRecorder : IDisposable
{
    internal const int WmKeyDown = 0x0100;
    internal const int WmMouseMove = 0x0200;
    internal const int WmLButtonDown = 0x0201;
    internal const int WmLButtonUp = 0x0202;
    internal const int WmLButtonDblClk = 0x0203;
    internal const int WmRButtonDown = 0x0204;
    internal const int WmRButtonUp = 0x0205;
    internal const int WmMButtonDown = 0x0207;
    internal const int WmMButtonUp = 0x0208;
    internal const int WmMouseWheel = 0x020A;
    internal const int WmMouseHWheel = 0x020E;
    internal const int WmAppCommand = 0x0319;
    internal const int MkLButton = 0x0001;
    internal const int MkControl = 0x0008;

    // The dwExtraInfo tag Dwell Click stamps on every event it injects (INJECTION_TAG in dllmain.cpp).
    private const long DwellClickInjectionTag = 0x44574C43;
    private const int WhKeyboardLl = 13;
    private const int LlkhfUp = 0x80;

    private readonly ConcurrentQueue<Message> messages = new();
    private readonly ConcurrentQueue<(int Vk, bool Up)> injectedKeys = new();
    private readonly TaskCompletionSource<RecorderForm> ready = new(TaskCreationOptions.RunContinuationsAsynchronously);
    private readonly Thread thread;
    private HookProc? keyboardHookProc;
    private IntPtr keyboardHook;
    private RecorderForm? form;
    private bool disposed;

    internal readonly record struct Message(int Id, int X, int Y, long WParam, long LParam);

    internal DwellClickActionRecorder(Rectangle bounds)
    {
        thread = new Thread(() => Run(bounds))
        {
            IsBackground = true,
            Name = "Dwell Click action recorder",
        };
        thread.SetApartmentState(ApartmentState.STA);
        thread.Start();
        form = ready.Task.WaitAsync(TimeSpan.FromSeconds(15)).GetAwaiter().GetResult();
    }

    internal IntPtr Handle => Invoke(() => form!.Handle);

    /// <summary>The window's outer bounds in screen coordinates.</summary>
    internal Rectangle Bounds => Invoke(() => form!.Bounds);

    /// <summary>The client area in screen coordinates: where clicks and wheel notches land.</summary>
    internal Rectangle ClientBounds => Invoke(() => form!.RectangleToScreen(form.ClientRectangle));

    internal IReadOnlyList<Message> Messages => messages.ToArray();

    /// <summary>Keystrokes Dwell Click injected since the last Clear, in order.</summary>
    internal IReadOnlyList<(int Vk, bool Up)> InjectedKeys => injectedKeys.ToArray();

    internal void Clear()
    {
        messages.Clear();
        injectedKeys.Clear();
    }

    internal int Count(int id) => messages.Count(m => m.Id == id);

    internal IReadOnlyList<Message> All(int id) => messages.Where(m => m.Id == id).ToList();

    internal string Describe() =>
        string.Join(", ", messages.Where(m => m.Id != WmMouseMove).Select(m => $"0x{m.Id:X4}@({m.X},{m.Y}) w=0x{m.WParam:X}"));

    internal void Invoke(Action action) => Invoke(() =>
    {
        action();
        return true;
    });

    internal T Invoke<T>(Func<T> action)
    {
        ObjectDisposedException.ThrowIf(disposed, this);
        var window = form ?? throw new InvalidOperationException("The recorder is not ready.");
        var completion = new TaskCompletionSource<T>(TaskCreationOptions.RunContinuationsAsynchronously);
        window.BeginInvoke(new Action(() =>
        {
            try
            {
                completion.SetResult(action());
            }
            catch (Exception ex)
            {
                completion.SetException(ex);
            }
        }));
        return completion.Task.WaitAsync(TimeSpan.FromSeconds(15)).GetAwaiter().GetResult();
    }

    public void Dispose()
    {
        if (disposed)
        {
            return;
        }

        if (form is { IsDisposed: false })
        {
            Invoke(() => form.Close());
        }

        disposed = true;
        thread.Join(TimeSpan.FromSeconds(10));
    }

    private void Run(Rectangle bounds)
    {
        try
        {
            using var window = new RecorderForm(messages)
            {
                Text = "Dwell Click action recorder",
                StartPosition = Forms.FormStartPosition.Manual,
                Bounds = bounds,
                BackColor = Color.White,
            };
            window.Shown += (_, _) => ready.TrySetResult(window);

            // A low-level keyboard hook on this (pumping) thread sees every injected key before
            // the shell acts on it.
            keyboardHookProc = OnKeyboardHook;
            keyboardHook = SetWindowsHookExW(WhKeyboardLl, keyboardHookProc, GetModuleHandleW(null), 0);
            try
            {
                Forms.Application.Run(window);
            }
            finally
            {
                if (keyboardHook != IntPtr.Zero)
                {
                    UnhookWindowsHookEx(keyboardHook);
                    keyboardHook = IntPtr.Zero;
                }
            }
        }
        catch (Exception ex)
        {
            ready.TrySetException(ex);
        }
    }

    private IntPtr OnKeyboardHook(int code, IntPtr wParam, IntPtr lParam)
    {
        if (code >= 0)
        {
            var info = Marshal.PtrToStructure<KbdLlHookStruct>(lParam);
            if (info.ExtraInfo.ToInt64() == DwellClickInjectionTag)
            {
                injectedKeys.Enqueue(((int)info.VkCode, (info.Flags & LlkhfUp) != 0));
            }
        }

        return CallNextHookEx(IntPtr.Zero, code, wParam, lParam);
    }

    private delegate IntPtr HookProc(int code, IntPtr wParam, IntPtr lParam);

    [StructLayout(LayoutKind.Sequential)]
    private struct KbdLlHookStruct
    {
        public uint VkCode;
        public uint ScanCode;
        public uint Flags;
        public uint Time;
        public IntPtr ExtraInfo;
    }

    [DllImport("user32.dll", SetLastError = true)]
    private static extern IntPtr SetWindowsHookExW(int idHook, HookProc lpfn, IntPtr hMod, uint dwThreadId);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool UnhookWindowsHookEx(IntPtr hhk);

    [DllImport("user32.dll")]
    private static extern IntPtr CallNextHookEx(IntPtr hhk, int nCode, IntPtr wParam, IntPtr lParam);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr GetModuleHandleW(string? moduleName);

    private sealed class RecorderForm : Forms.Form
    {
        private readonly ConcurrentQueue<Message> sink;

        internal RecorderForm(ConcurrentQueue<Message> sink)
        {
            this.sink = sink;
        }

        protected override void WndProc(ref Forms.Message m)
        {
            var id = m.Msg;
            var wParam = m.WParam.ToInt64();
            switch (id)
            {
                case WmLButtonDown:
                case WmLButtonUp:
                case WmLButtonDblClk:
                case WmRButtonDown:
                case WmRButtonUp:
                case WmMButtonDown:
                case WmMButtonUp:
                    sink.Enqueue(Record(id, wParam, m.LParam, clientCoordinates: true));
                    break;
                case WmMouseMove:
                    // Only moves with the left button down: they are what a drag looks like.
                    if ((wParam & MkLButton) != 0)
                    {
                        sink.Enqueue(Record(id, wParam, m.LParam, clientCoordinates: true));
                    }

                    break;
                case WmMouseWheel:
                case WmMouseHWheel:
                    // Wheel messages carry screen coordinates.
                    sink.Enqueue(Record(id, wParam, m.LParam, clientCoordinates: false));
                    break;
                case WmKeyDown:
                case WmAppCommand:
                    sink.Enqueue(new Message(id, 0, 0, wParam, m.LParam.ToInt64()));
                    break;
            }

            base.WndProc(ref m);
        }

        private Message Record(int id, long wParam, IntPtr lParam, bool clientCoordinates)
        {
            var packed = lParam.ToInt64();
            var x = (short)(packed & 0xFFFF);
            var y = (short)((packed >> 16) & 0xFFFF);
            var point = clientCoordinates ? PointToScreen(new Point(x, y)) : new Point(x, y);
            return new Message(id, point.X, point.Y, wParam, packed);
        }
    }
}

// Audio Holder v6 - tiny native Windows app, C# version (no dependencies)
//
// Logic:
//   Runs in the tray and waits for cs2.exe. When it appears, the chosen audio device is
//   kept busy with an inaudible looping stream for 20 seconds, then released. After that
//   the app rests until cs2.exe has closed, and then goes back to waiting for the next start.
//
// Optional settings in %APPDATA%\AudioDeviceBeeper\config.txt (plain text, key=value):
//   process=cs2            comma separated list of process names that trigger the hold
//   hold=20                seconds to hold the device after the trigger process appears
//
// Build: run build.bat, or
//   C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe /target:winexe /optimize+ /out:Audio_holder_v6_cs.exe /reference:System.Windows.Forms.dll /reference:System.Drawing.dll Audio_holder_v6.cs

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;
using System.Windows.Forms;
using Microsoft.Win32;

namespace AudioHolder
{
    internal static class NativeMethods
    {
        [StructLayout(LayoutKind.Sequential, Pack = 1)]
        public struct WAVEFORMATEX
        {
            public ushort wFormatTag;
            public ushort nChannels;
            public uint nSamplesPerSec;
            public uint nAvgBytesPerSec;
            public ushort nBlockAlign;
            public ushort wBitsPerSample;
            public ushort cbSize;
        }

        [StructLayout(LayoutKind.Sequential, Pack = 1)]
        public struct WAVEHDR
        {
            public IntPtr lpData;
            public uint dwBufferLength;
            public uint dwBytesRecorded;
            public IntPtr dwUser;
            public uint dwFlags;
            public uint dwLoops;
            public IntPtr lpNext;
            public IntPtr reserved;
        }

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Auto, Pack = 1)]
        public struct WAVEOUTCAPS
        {
            public ushort wMid;
            public ushort wPid;
            public uint vDriverVersion;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)]
            public string szPname;
            public uint dwFormats;
            public ushort wChannels;
            public ushort wReserved1;
            public uint dwSupport;
        }

        [DllImport("winmm.dll")]
        public static extern uint waveOutGetNumDevs();

        [DllImport("winmm.dll", CharSet = CharSet.Auto)]
        public static extern uint waveOutGetDevCaps(UIntPtr uDeviceID, ref WAVEOUTCAPS pwoc, uint cbwoc);

        [DllImport("winmm.dll")]
        public static extern uint waveOutOpen(out IntPtr hwo, uint uDeviceID, ref WAVEFORMATEX lpFormat,
                                              IntPtr dwCallback, IntPtr dwInstance, uint dwFlags);

        [DllImport("winmm.dll")]
        public static extern uint waveOutPrepareHeader(IntPtr hwo, IntPtr pwh, uint cbwh);

        [DllImport("winmm.dll")]
        public static extern uint waveOutUnprepareHeader(IntPtr hwo, IntPtr pwh, uint cbwh);

        [DllImport("winmm.dll")]
        public static extern uint waveOutWrite(IntPtr hwo, IntPtr pwh, uint cbwh);

        [DllImport("winmm.dll")]
        public static extern uint waveOutReset(IntPtr hwo);

        [DllImport("winmm.dll")]
        public static extern uint waveOutClose(IntPtr hwo);

        [DllImport("user32.dll")]
        public static extern bool DestroyIcon(IntPtr handle);

        [DllImport("kernel32.dll")]
        public static extern bool SetProcessWorkingSetSize(IntPtr hProcess, IntPtr dwMinimumWorkingSetSize, IntPtr dwMaximumWorkingSetSize);
    }

    internal static class Program
    {
        [STAThread]
        private static void Main(string[] args)
        {
            bool created;
            using (Mutex mutex = new Mutex(true, "AudioHolder_v6_SingleInstance", out created))
            {
                if (!created)
                {
                    return; // already running (it lives in the tray)
                }
                Application.EnableVisualStyles();
                Application.SetCompatibleTextRenderingDefault(false);
                Application.Run(new MainForm(args));
            }
        }
    }

    internal class MainForm : Form
    {
        // config folder and autostart entry keep the name used by the v5.x builds,
        // so the saved device carries over and the old autostart entry is replaced
        private const string AppName = "AudioDeviceBeeper";
        private const string RunKey = @"Software\Microsoft\Windows\CurrentVersion\Run";
        private const int SampleRate = 22050;
        private const uint WHDR_DONE = 0x1;
        private const uint WHDR_BEGINLOOP = 0x4;
        private const uint WHDR_ENDLOOP = 0x8;
        private const uint LoopCount = 0x7FFFFFFF; // effectively forever

        private enum State { Waiting, Holding, Resting }

        private static readonly string ConfigPath = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
            Path.Combine(AppName, "config.txt"));

        private ComboBox combo;
        private CheckBox watchBox;
        private CheckBox autostartBox;
        private Label statusLabel;
        private NotifyIcon tray;
        private ToolStripMenuItem watchItem;
        private System.Windows.Forms.Timer timer;

        private bool allowVisible = true;
        private bool exiting = false;
        private bool filling = false;
        private bool syncing = false;
        private string lastTip = "";

        // settings
        private string savedDevice = "";
        private bool autostart = true;
        private bool watchOn = true;
        private string processText = "cs2";
        private int holdSeconds = 20;
        private List<string> targets = new List<string>(); // process names without ".exe"

        // state machine
        private State state = State.Waiting;
        private DateTime gameSince = DateTime.UtcNow;

        // audio stream state
        private bool holding = false;
        private IntPtr hwo = IntPtr.Zero;
        private IntPtr hdrPtr = IntPtr.Zero;
        private IntPtr dataPtr = IntPtr.Zero;
        private uint hdrSize = (uint)Marshal.SizeOf(typeof(NativeMethods.WAVEHDR));

        public MainForm(string[] args)
        {
            LoadConfig();
            ParseTargets();

            Text = "Audio Holder v6";
            FormBorderStyle = FormBorderStyle.FixedSingle;
            MaximizeBox = false;
            StartPosition = FormStartPosition.CenterScreen;
            ClientSize = new Size(440, 165);
            Font = new Font("Segoe UI", 9f);

            Label label = new Label();
            label.Text = "Audio output device:";
            label.Location = new Point(16, 14);
            label.AutoSize = true;
            Controls.Add(label);

            combo = new ComboBox();
            combo.DropDownStyle = ComboBoxStyle.DropDownList;
            combo.Location = new Point(16, 36);
            combo.Width = 408;
            combo.SelectedIndexChanged += delegate { OnDeviceChanged(); };
            Controls.Add(combo);

            watchBox = new CheckBox();
            watchBox.Text = "Watch for " + TargetsLabel() + " and hold the device for " + holdSeconds + " s";
            watchBox.Location = new Point(16, 76);
            watchBox.AutoSize = true;
            watchBox.Checked = watchOn;
            watchBox.CheckedChanged += delegate { if (!syncing) { SetWatch(watchBox.Checked); } };
            Controls.Add(watchBox);

            autostartBox = new CheckBox();
            autostartBox.Text = "Start with Windows";
            autostartBox.Location = new Point(16, 102);
            autostartBox.AutoSize = true;
            autostartBox.Checked = autostart;
            autostartBox.CheckedChanged += delegate { OnAutostartChanged(); };
            Controls.Add(autostartBox);

            statusLabel = new Label();
            statusLabel.Location = new Point(16, 132);
            statusLabel.Size = new Size(408, 20);
            statusLabel.AutoEllipsis = true;
            Controls.Add(statusLabel);

            FillDevices();

            Icon icon = MakeIcon();
            Icon = icon;

            watchItem = new ToolStripMenuItem("Watch for game");
            watchItem.CheckOnClick = true;
            watchItem.Checked = watchOn;
            watchItem.Click += delegate { if (!syncing) { SetWatch(watchItem.Checked); } };

            ContextMenuStrip menu = new ContextMenuStrip();
            menu.Items.Add(watchItem);
            menu.Items.Add(new ToolStripSeparator());
            menu.Items.Add("Show", null, delegate { ShowFromTray(); });
            menu.Items.Add("Exit", null, delegate { ExitApp(); });

            tray = new NotifyIcon();
            tray.Icon = icon;
            tray.Text = "Audio Holder v6";
            tray.ContextMenuStrip = menu;
            tray.Visible = true;
            tray.MouseClick += delegate(object s, MouseEventArgs e)
            {
                if (e.Button == MouseButtons.Left) { ShowFromTray(); }
            };

            // keep registry in sync (creates the entry on first run)
            try { SetAutostart(autostartBox.Checked); } catch { }
            SaveConfig();

            // check once a second
            timer = new System.Windows.Forms.Timer();
            timer.Interval = 1000;
            timer.Tick += delegate { HoldTick(); };

            ResetState();
            HoldTick();
            timer.Start();

            // trim memory once, a few seconds after startup
            System.Windows.Forms.Timer startTrim = new System.Windows.Forms.Timer();
            startTrim.Interval = 3000;
            startTrim.Tick += delegate
            {
                startTrim.Stop();
                startTrim.Dispose();
                TrimMemory();
            };
            startTrim.Start();

            SystemEvents.PowerModeChanged += OnPowerModeChanged;

            if (Array.IndexOf(args, "--minimized") >= 0)
            {
                allowVisible = false; // start hidden in the tray
            }

            Resize += delegate
            {
                if (WindowState == FormWindowState.Minimized) { HideToTray(); }
            };
        }

        protected override void SetVisibleCore(bool value)
        {
            if (!allowVisible)
            {
                value = false;
                if (!IsHandleCreated) { CreateHandle(); }
            }
            base.SetVisibleCore(value);
        }

        protected override void OnFormClosing(FormClosingEventArgs e)
        {
            if (!exiting && e.CloseReason == CloseReason.UserClosing)
            {
                e.Cancel = true;
                SaveConfig();
                HideToTray();
                return;
            }
            base.OnFormClosing(e);
        }

        private void HideToTray()
        {
            Hide();
            TrimMemory();
        }

        private void ShowFromTray()
        {
            allowVisible = true;
            Show();
            WindowState = FormWindowState.Normal;
            Activate();
        }

        private void ExitApp()
        {
            exiting = true;
            SystemEvents.PowerModeChanged -= OnPowerModeChanged;
            timer.Stop();
            StopHold();
            SaveConfig();
            tray.Visible = false;
            tray.Dispose();
            Application.Exit();
        }

        private void OnPowerModeChanged(object sender, PowerModeChangedEventArgs e)
        {
            // after sleep the old stream may be dead: drop it, the next tick reopens it if needed
            if (e.Mode == PowerModes.Resume && IsHandleCreated && !exiting)
            {
                BeginInvoke(new MethodInvoker(delegate { StopHold(); }));
            }
        }

        // give unused memory back to Windows (Task Manager shows a smaller number)
        private static void TrimMemory()
        {
            try
            {
                GC.Collect();
                GC.WaitForPendingFinalizers();
                GC.Collect();
                using (Process self = Process.GetCurrentProcess())
                {
                    NativeMethods.SetProcessWorkingSetSize(self.Handle, new IntPtr(-1), new IntPtr(-1));
                }
            }
            catch { }
        }

        // ---------- watching on/off ----------
        private void SetWatch(bool on)
        {
            watchOn = on;
            syncing = true;
            watchBox.Checked = on;
            watchItem.Checked = on;
            syncing = false;
            SaveConfig();
            ResetState();
            HoldTick();
            if (!on) { TrimMemory(); }
        }

        // ---------- config ----------
        private void LoadConfig()
        {
            try
            {
                if (!File.Exists(ConfigPath)) { return; }
                foreach (string line in File.ReadAllLines(ConfigPath))
                {
                    int p = line.IndexOf('=');
                    if (p <= 0) { continue; }
                    string key = line.Substring(0, p);
                    string val = line.Substring(p + 1);
                    if (key == "device") { savedDevice = val; }
                    else if (key == "autostart") { autostart = (val.Trim() == "1"); }
                    else if (key == "watch") { watchOn = (val.Trim() == "1"); }
                    else if (key == "process") { processText = val.Trim(); }
                    else if (key == "hold")
                    {
                        int h;
                        if (int.TryParse(val.Trim(), out h) && h >= 1 && h <= 600) { holdSeconds = h; }
                    }
                }
            }
            catch { }
        }

        private void SaveConfig()
        {
            try
            {
                bool auto = autostartBox != null ? autostartBox.Checked : autostart;
                Directory.CreateDirectory(Path.GetDirectoryName(ConfigPath));
                File.WriteAllLines(ConfigPath, new string[]
                {
                    "device=" + savedDevice,
                    "autostart=" + (auto ? "1" : "0"),
                    "watch=" + (watchOn ? "1" : "0"),
                    "process=" + processText,
                    "hold=" + holdSeconds
                });
            }
            catch { }
        }

        // "cs2, other.exe" -> { "cs2", "other" }
        private void ParseTargets()
        {
            targets.Clear();
            foreach (string part in processText.Split(','))
            {
                string name = part.Trim().ToLowerInvariant();
                if (name.EndsWith(".exe")) { name = name.Substring(0, name.Length - 4); }
                if (name.Length > 0) { targets.Add(name); }
            }
            if (targets.Count == 0) { targets.Add("cs2"); }
        }

        private string TargetsLabel()
        {
            List<string> names = new List<string>();
            foreach (string t in targets) { names.Add(t + ".exe"); }
            return string.Join(", ", names.ToArray());
        }

        // ---------- autostart ----------
        private static void SetAutostart(bool enable)
        {
            using (RegistryKey key = Registry.CurrentUser.OpenSubKey(RunKey, true))
            {
                if (key == null) { return; }
                if (enable)
                {
                    key.SetValue(AppName, "\"" + Application.ExecutablePath + "\" --minimized");
                }
                else
                {
                    key.DeleteValue(AppName, false);
                }
            }
        }

        private void OnAutostartChanged()
        {
            try
            {
                SetAutostart(autostartBox.Checked);
            }
            catch (Exception ex)
            {
                MessageBox.Show("Could not change autostart:\n" + ex.Message, "Autostart");
            }
            SaveConfig();
        }

        // ---------- devices ----------
        private static List<string> GetDevices()
        {
            List<string> names = new List<string>();
            uint n = NativeMethods.waveOutGetNumDevs();
            for (uint i = 0; i < n; i++)
            {
                NativeMethods.WAVEOUTCAPS caps = new NativeMethods.WAVEOUTCAPS();
                uint r = NativeMethods.waveOutGetDevCaps(new UIntPtr(i), ref caps,
                    (uint)Marshal.SizeOf(typeof(NativeMethods.WAVEOUTCAPS)));
                names.Add(r == 0 ? caps.szPname : "Device " + i);
            }
            return names;
        }

        private void FillDevices()
        {
            filling = true;
            List<string> names = GetDevices();
            combo.Items.Clear();
            foreach (string name in names) { combo.Items.Add(name); }

            int sel = names.IndexOf(savedDevice);
            if (sel < 0 && savedDevice.Length == 0 && names.Count > 0)
            {
                sel = 0; // first run: pick the first device
                savedDevice = names[0];
            }
            if (sel >= 0) { combo.SelectedIndex = sel; }
            filling = false;
        }

        private void OnDeviceChanged()
        {
            if (filling || combo.SelectedItem == null) { return; }
            savedDevice = (string)combo.SelectedItem;
            SaveConfig();
            StopHold();   // reopened on the next tick if a hold is due
            HoldTick();
        }

        // ---------- holding the device ----------
        // 1 second of inaudible near-silence (random -1/0/+1 values), looped.
        private static byte[] MakeQuiet()
        {
            byte[] buf = new byte[SampleRate * 2];
            Random rnd = new Random(12345);
            for (int i = 0; i < SampleRate; i++)
            {
                short v = (short)rnd.Next(-1, 2);
                buf[2 * i] = (byte)(v & 0xFF);
                buf[2 * i + 1] = (byte)((v >> 8) & 0xFF);
            }
            return buf;
        }

        private bool StartHold()
        {
            if (savedDevice.Length == 0) { return false; }

            // look the device up by name, the index can change
            int dev = GetDevices().IndexOf(savedDevice);
            if (dev < 0) { return false; }

            NativeMethods.WAVEFORMATEX fmt = new NativeMethods.WAVEFORMATEX();
            fmt.wFormatTag = 1; // PCM
            fmt.nChannels = 1;
            fmt.nSamplesPerSec = SampleRate;
            fmt.wBitsPerSample = 16;
            fmt.nBlockAlign = 2;
            fmt.nAvgBytesPerSec = SampleRate * 2;
            fmt.cbSize = 0;

            IntPtr h;
            uint r = NativeMethods.waveOutOpen(out h, (uint)dev, ref fmt, IntPtr.Zero, IntPtr.Zero, 0);
            if (r != 0) { return false; }
            hwo = h;

            byte[] pcm = MakeQuiet();
            dataPtr = Marshal.AllocHGlobal(pcm.Length);
            Marshal.Copy(pcm, 0, dataPtr, pcm.Length);

            NativeMethods.WAVEHDR hdr = new NativeMethods.WAVEHDR();
            hdr.lpData = dataPtr;
            hdr.dwBufferLength = (uint)pcm.Length;
            hdrPtr = Marshal.AllocHGlobal((int)hdrSize);
            Marshal.StructureToPtr(hdr, hdrPtr, false);

            r = NativeMethods.waveOutPrepareHeader(hwo, hdrPtr, hdrSize);
            if (r == 0)
            {
                // set the loop flags after preparing the header
                hdr = (NativeMethods.WAVEHDR)Marshal.PtrToStructure(hdrPtr, typeof(NativeMethods.WAVEHDR));
                hdr.dwFlags |= WHDR_BEGINLOOP | WHDR_ENDLOOP;
                hdr.dwLoops = LoopCount;
                Marshal.StructureToPtr(hdr, hdrPtr, false);
                r = NativeMethods.waveOutWrite(hwo, hdrPtr, hdrSize);
            }
            if (r != 0)
            {
                StopHold();
                return false;
            }

            holding = true;
            return true;
        }

        private void StopHold()
        {
            if (hwo != IntPtr.Zero)
            {
                NativeMethods.waveOutReset(hwo);
                if (hdrPtr != IntPtr.Zero)
                {
                    NativeMethods.waveOutUnprepareHeader(hwo, hdrPtr, hdrSize);
                }
                NativeMethods.waveOutClose(hwo);
                hwo = IntPtr.Zero;
            }
            if (hdrPtr != IntPtr.Zero) { Marshal.FreeHGlobal(hdrPtr); hdrPtr = IntPtr.Zero; }
            if (dataPtr != IntPtr.Zero) { Marshal.FreeHGlobal(dataPtr); dataPtr = IntPtr.Zero; }
            holding = false;
        }

        private bool HoldIsAlive()
        {
            if (!holding || hdrPtr == IntPtr.Zero) { return false; }

            // device gone from the system list?
            if (GetDevices().IndexOf(savedDevice) < 0) { return false; }

            NativeMethods.WAVEHDR hdr = (NativeMethods.WAVEHDR)Marshal.PtrToStructure(hdrPtr, typeof(NativeMethods.WAVEHDR));
            return (hdr.dwFlags & WHDR_DONE) == 0;
        }

        // make sure the device is held; reopen it if it vanished or the stream ended
        private void EnsureHold()
        {
            if (holding)
            {
                if (HoldIsAlive()) { return; }
                StopHold();
            }
            if (!StartHold())
            {
                // device not there (yet): refresh the list if it changed
                if (GetDevices().Count != combo.Items.Count) { FillDevices(); }
            }
        }

        // ---------- game detection ----------
        private bool GameRunning()
        {
            foreach (string name in targets)
            {
                Process[] list = null;
                try
                {
                    list = Process.GetProcessesByName(name);
                    if (list.Length > 0) { return true; }
                }
                catch { }
                finally
                {
                    if (list != null)
                    {
                        foreach (Process p in list) { p.Dispose(); }
                    }
                }
            }
            return false;
        }

        // ---------- state machine ----------
        // at startup / when watching is switched on: if the game is already running, just rest
        private void ResetState()
        {
            StopHold();
            state = (watchOn && GameRunning()) ? State.Resting : State.Waiting;
        }

        private void HoldTick()
        {
            if (exiting) { return; }

            if (!watchOn)
            {
                StopHold();
                state = State.Waiting;
                UpdateStatus();
                return;
            }

            bool running = GameRunning();

            if (state == State.Waiting)
            {
                if (running)
                {
                    state = State.Holding;
                    gameSince = DateTime.UtcNow;
                    EnsureHold();
                }
            }
            else if (state == State.Holding)
            {
                if (!running)
                {
                    StopHold();           // game closed again during the hold window
                    state = State.Waiting;
                }
                else if ((DateTime.UtcNow - gameSince).TotalSeconds >= holdSeconds)
                {
                    StopHold();           // the game has its own audio stream now
                    state = State.Resting;
                    TrimMemory();
                }
                else
                {
                    EnsureHold();
                }
            }
            else // Resting
            {
                if (!running) { state = State.Waiting; }
            }

            UpdateStatus();
        }

        private void UpdateStatus()
        {
            if (statusLabel == null) { return; }

            string text;
            string shortState;
            if (!watchOn)
            {
                text = "Status: watching is off";
                shortState = "off";
            }
            else if (state == State.Waiting)
            {
                text = "Status: waiting for " + TargetsLabel();
                shortState = "waiting";
            }
            else if (state == State.Holding)
            {
                int left = holdSeconds - (int)(DateTime.UtcNow - gameSince).TotalSeconds;
                if (left < 0) { left = 0; }
                if (holding)
                {
                    text = "Status: game detected - holding \"" + savedDevice + "\" (" + left + " s left)";
                }
                else
                {
                    text = "Status: game detected - waiting for the device...";
                }
                shortState = "holding";
            }
            else
            {
                text = "Status: game running - device released";
                shortState = "resting";
            }

            if (statusLabel.Text != text) { statusLabel.Text = text; }

            string tip = "Audio Holder v6 - " + shortState;
            if (tip != lastTip)
            {
                lastTip = tip;
                tray.Text = tip;
            }
        }

        // ---------- icon ----------
        private static Icon MakeIcon()
        {
            using (Bitmap bmp = new Bitmap(32, 32))
            {
                using (Graphics g = Graphics.FromImage(bmp))
                {
                    g.SmoothingMode = SmoothingMode.AntiAlias;
                    g.Clear(Color.Transparent);
                    using (Brush b = new SolidBrush(Color.FromArgb(40, 110, 220)))
                    {
                        g.FillEllipse(b, 1, 1, 30, 30);
                    }
                    Point[] tri = new Point[] { new Point(12, 9), new Point(12, 23), new Point(23, 16) };
                    g.FillPolygon(Brushes.White, tri);
                }
                IntPtr hIcon = bmp.GetHicon();
                Icon tmp = Icon.FromHandle(hIcon);
                Icon copy = (Icon)tmp.Clone();
                NativeMethods.DestroyIcon(hIcon);
                return copy;
            }
        }
    }
}

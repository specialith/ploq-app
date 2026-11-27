// Ploq Setup: the one-screen Windows installer (tools/collab/installer/setup/README.md).
//
// A small WPF app in the lander's style (black, white, Inter, sharp corners, a white button) that
// carries the real installer, Ploq-<version>.msi, inside it and runs it with Windows Installer's own
// API (MsiInstallProduct) with no Windows Installer UI, showing its own progress. The .msi still does
// the actual work (per-user install, no admin prompt, Start menu, ploq:// links, uninstall entry), and
// auto-updates keep using the .msi directly.
//
// Written for the C# 5 compiler and WPF that come with Windows (.NET Framework 4.8), so building it
// needs nothing installed: tools/collab/installer/setup/build_setup.ps1.
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Threading;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Documents;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Shapes;
using Microsoft.Win32;

namespace PloqSetup
{
    internal static class Msi
    {
        public delegate int UiHandler(IntPtr context, uint messageType, [MarshalAs(UnmanagedType.LPWStr)] string message);

        [DllImport("msi.dll", CharSet = CharSet.Unicode)]
        public static extern int MsiSetInternalUI(int uiLevel, IntPtr window);

        [DllImport("msi.dll", CharSet = CharSet.Unicode)]
        public static extern UiHandler MsiSetExternalUI(UiHandler handler, uint messageFilter, IntPtr context);

        [DllImport("msi.dll", CharSet = CharSet.Unicode)]
        public static extern uint MsiInstallProduct(string packagePath, string commandLine);

        [DllImport("msi.dll", CharSet = CharSet.Unicode)]
        public static extern uint MsiEnableLog(uint logMode, string logFile, uint logAttributes);

        public const int INSTALLUILEVEL_NONE = 2;
        public const uint INSTALLMESSAGE_ERROR = 0x01000000;
        public const uint INSTALLMESSAGE_WARNING = 0x02000000;
        public const uint INSTALLMESSAGE_PROGRESS = 0x0A000000;
        public const uint INSTALLLOGMODE_ERROR = 1u << 1;
        public const uint INSTALLLOGMODE_WARNING = 1u << 2;
        public const uint INSTALLLOGMODE_PROGRESS = 1u << 10;
        public const uint INSTALLLOGMODE_VERBOSE = 1u << 12;
    }

    //! A plain rectangle-and-text button drawn the way the lander draws .button: white with black text,
    //! inverting on hover; disabled = a hairline outline with dim text.
    internal sealed class FlatButton : Border
    {
        private readonly TextBlock m_text = new TextBlock();
        private bool m_enabled = true;
        private bool m_hover;
        public event EventHandler Click;

        public FlatButton(string text, FontFamily font)
        {
            Width = 148;
            Height = 50;
            BorderThickness = new Thickness(1);
            Cursor = Cursors.Hand;
            m_text.Text = text;
            m_text.FontFamily = font;
            m_text.FontSize = 17;
            m_text.HorizontalAlignment = HorizontalAlignment.Center;
            m_text.VerticalAlignment = VerticalAlignment.Center;
            Child = m_text;
            MouseEnter += delegate { m_hover = true; Paint(); };
            MouseLeave += delegate { m_hover = false; Paint(); };
            MouseLeftButtonUp += delegate(object s, MouseButtonEventArgs e) { e.Handled = true; Fire(); };
            MouseLeftButtonDown += delegate(object s, MouseButtonEventArgs e) { e.Handled = true; };
            Paint();
        }

        public string Text { set { m_text.Text = value; } }

        public bool Active
        {
            get { return m_enabled; }
            set { m_enabled = value; Cursor = value ? Cursors.Hand : Cursors.Arrow; Paint(); }
        }

        public void Fire()
        {
            if (m_enabled && Click != null) {
                Click(this, EventArgs.Empty);
            }
        }

        private void Paint()
        {
            if (!m_enabled) {
                Background = Brushes.Transparent;
                BorderBrush = Setup.Line;
                m_text.Foreground = Setup.Dim;
            } else if (m_hover) {
                Background = Brushes.Black;
                BorderBrush = Brushes.White;
                m_text.Foreground = Brushes.White;
            } else {
                Background = Brushes.White;
                BorderBrush = Brushes.White;
                m_text.Foreground = Brushes.Black;
            }
        }
    }

    internal sealed class Setup : Window
    {
        public static readonly Brush Muted = Freeze(new SolidColorBrush(Color.FromRgb(143, 143, 143))); // 56% white on black
        public static readonly Brush Line = Freeze(new SolidColorBrush(Color.FromRgb(36, 36, 36)));     // 14% white on black
        public static readonly Brush Dim = Freeze(new SolidColorBrush(Color.FromRgb(90, 90, 90)));

        private const double W = 680, H = 380, PAD = 44, RULE_Y = 236;

        private readonly string m_version;
        private readonly string m_work;   // temp folder: fonts and the extracted .msi
        private readonly FontFamily m_light, m_regular;
        private string m_installDir;
        private bool m_agreed;
        private bool m_installing;

        private readonly Canvas m_root = new Canvas();
        private readonly TextBlock m_path = new TextBlock();
        private readonly TextBlock m_change = new TextBlock();
        private readonly TextBlock m_installTo = new TextBlock();
        private readonly Border m_box = new Border();
        private readonly System.Windows.Shapes.Path m_tick = new System.Windows.Shapes.Path();
        private readonly TextBlock m_agree = new TextBlock();
        private readonly TextBlock m_status = new TextBlock();
        private readonly Rectangle m_bar = new Rectangle();
        private readonly FlatButton m_button;
        private readonly TextBlock m_close = new TextBlock();
        private Action m_buttonAction;

        private static Brush Freeze(Brush b) { b.Freeze(); return b; }

        public Setup()
        {
            m_version = ReadText("version.txt").Trim();
            m_work = System.IO.Path.Combine(System.IO.Path.GetTempPath(), "PloqSetup-" + Guid.NewGuid().ToString("N").Substring(0, 8));
            Directory.CreateDirectory(m_work);
            string fonts = System.IO.Path.Combine(m_work, "fonts");
            Directory.CreateDirectory(fonts);
            Extract("Inter-Light.ttf", System.IO.Path.Combine(fonts, "Inter-Light.ttf"));
            Extract("Inter-Regular.ttf", System.IO.Path.Combine(fonts, "Inter-Regular.ttf"));
            Uri fontDir = new Uri(fonts + System.IO.Path.DirectorySeparatorChar);
            m_light = new FontFamily(fontDir, "./#Inter Light");
            m_regular = new FontFamily(fontDir, "./#Inter");
            m_installDir = DefaultInstallDir();

            Title = "Ploq Setup";
            Width = W;
            Height = H;
            ResizeMode = ResizeMode.NoResize;
            WindowStyle = WindowStyle.None;
            WindowStartupLocation = WindowStartupLocation.CenterScreen;
            Background = Brushes.Black;
            BorderBrush = Line;
            BorderThickness = new Thickness(1);
            UseLayoutRounding = true;
            TextOptions.SetTextFormattingMode(this, TextFormattingMode.Ideal);
            try {
                Icon = BitmapFrame.Create(Resource("Ploq.ico"));
            } catch (Exception) {
                // the window works without its taskbar icon
            }

            m_root.ClipToBounds = true;
            m_root.Background = Brushes.Black; // hit-testable, so the window can be dragged anywhere
            Content = m_root;
            MouseLeftButtonDown += delegate(object s, MouseButtonEventArgs e) { if (e.ButtonState == MouseButtonState.Pressed && !InLink(e.OriginalSource as DependencyObject)) { try { DragMove(); } catch (InvalidOperationException) { } } };
            KeyDown += OnKey;
            Closing += OnClosing;
            Closed += delegate { Cleanup(); };

            // The blurred mark, big on the right, partly cut off by the window edge (a window-sized picture).
            Image mark = new Image();
            BitmapImage art = new BitmapImage();
            art.BeginInit();
            art.StreamSource = Resource("setup-mark.png");
            art.CacheOption = BitmapCacheOption.OnLoad;
            art.EndInit();
            mark.Source = art;
            mark.Width = W;
            mark.Height = H;
            mark.IsHitTestVisible = false;
            Place(mark, 0, 0);

            // Close.
            m_close.Text = "✕";
            m_close.FontSize = 13;
            m_close.Foreground = Muted;
            m_close.Padding = new Thickness(10, 6, 10, 6);
            m_close.Cursor = Cursors.Hand;
            m_close.MouseEnter += delegate { if (!m_installing) m_close.Foreground = Brushes.White; };
            m_close.MouseLeave += delegate { m_close.Foreground = Muted; };
            m_close.MouseLeftButtonUp += delegate(object s, MouseButtonEventArgs e) { e.Handled = true; if (!m_installing) Close(); };
            m_close.MouseLeftButtonDown += delegate(object s, MouseButtonEventArgs e) { e.Handled = true; };
            Canvas.SetRight(m_close, 6);
            Canvas.SetTop(m_close, 6);
            m_root.Children.Add(m_close);

            // Wordmark and line.
            Place(Text("Ploq", m_light, 76, Brushes.White), PAD - 6, 18);
            Place(Text("Real-time collab for MuseScore", m_light, 18, Muted), PAD, 131);
            Rectangle rule = new Rectangle();
            rule.Width = W - 2 * PAD;
            rule.Height = 1;
            rule.Fill = Line;
            Place(rule, PAD, RULE_Y);
            m_bar.Height = 1;
            m_bar.Width = 0;
            m_bar.Fill = Brushes.White;
            Place(m_bar, PAD, RULE_Y);

            // Install to ... Change
            m_installTo.Text = "Install to";
            SetFont(m_installTo, m_regular, 13, Muted);
            Place(m_installTo, PAD, RULE_Y + 18);
            SetFont(m_path, m_regular, 13, Brushes.White);
            m_path.Width = W - 2 * PAD - 78 - 70;
            m_path.TextTrimming = TextTrimming.CharacterEllipsis;
            Place(m_path, PAD + 78, RULE_Y + 18);
            m_change.Text = "Change";
            SetFont(m_change, m_regular, 13, Brushes.White);
            m_change.TextDecorations = Underline();
            m_change.Cursor = Cursors.Hand;
            m_change.MouseLeftButtonUp += delegate(object s, MouseButtonEventArgs e) { e.Handled = true; ChooseFolder(); };
            m_change.MouseLeftButtonDown += delegate(object s, MouseButtonEventArgs e) { e.Handled = true; };
            Canvas.SetRight(m_change, PAD);
            Canvas.SetTop(m_change, RULE_Y + 18);
            m_root.Children.Add(m_change);
            ShowPath();

            // [ ] I agree to the Terms of Service
            m_box.Width = 16;
            m_box.Height = 16;
            m_box.BorderThickness = new Thickness(1);
            m_box.Cursor = Cursors.Hand;
            m_tick.Data = Geometry.Parse("M 3.5,8 L 6.5,11 L 12.5,4.5");
            m_tick.Stroke = Brushes.Black;
            m_tick.StrokeThickness = 2;
            m_box.Child = m_tick;
            m_box.MouseLeftButtonUp += delegate(object s, MouseButtonEventArgs e) { e.Handled = true; ToggleAgree(); };
            m_box.MouseLeftButtonDown += delegate(object s, MouseButtonEventArgs e) { e.Handled = true; };
            Place(m_box, PAD, RULE_Y + 81);
            SetFont(m_agree, m_regular, 15, Muted);
            m_agree.Inlines.Add(new Run("I agree to the "));
            Hyperlink terms = new Hyperlink(new Run("Terms of Service"));
            terms.Foreground = Brushes.White;
            terms.TextDecorations = Underline();
            terms.Cursor = Cursors.Hand;
            terms.Click += delegate { Open("https://ploq.app/terms"); };
            m_agree.Inlines.Add(terms);
            m_agree.Cursor = Cursors.Hand;
            m_agree.MouseLeftButtonUp += delegate(object s, MouseButtonEventArgs e) { if (!InLink(e.OriginalSource as DependencyObject)) { e.Handled = true; ToggleAgree(); } };
            m_agree.MouseLeftButtonDown += delegate(object s, MouseButtonEventArgs e) { e.Handled = true; };
            Place(m_agree, PAD + 28, RULE_Y + 78);

            // Status line (installing / done / problem), where the agree line was.
            SetFont(m_status, m_regular, 15, Brushes.White);
            m_status.Width = W - 2 * PAD - 148 - 24;
            m_status.TextWrapping = TextWrapping.Wrap;
            m_status.VerticalAlignment = VerticalAlignment.Center;
            m_status.Visibility = Visibility.Collapsed;
            Grid statusRow = new Grid(); // the button's height, so one line sits level with the button's text
            statusRow.Height = 50;
            statusRow.Children.Add(m_status);
            Place(statusRow, PAD, RULE_Y + 64);

            m_button = new FlatButton("Install", m_regular);
            m_button.Click += delegate { if (m_buttonAction != null) m_buttonAction(); };
            Canvas.SetRight(m_button, PAD);
            Canvas.SetTop(m_button, RULE_Y + 64);
            m_root.Children.Add(m_button);
            m_buttonAction = StartInstall;
            ShowAgree();
        }

        // --- layout helpers ------------------------------------------------------------------------

        private void Place(UIElement e, double x, double y)
        {
            Canvas.SetLeft(e, x);
            Canvas.SetTop(e, y);
            m_root.Children.Add(e);
        }

        private static void SetFont(TextBlock t, FontFamily f, double size, Brush fg)
        {
            t.FontFamily = f;
            t.FontSize = size;
            t.Foreground = fg;
        }

        private static TextBlock Text(string s, FontFamily f, double size, Brush fg)
        {
            TextBlock t = new TextBlock();
            t.Text = s;
            SetFont(t, f, size, fg);
            return t;
        }

        //! The lander's links: a thin muted underline, a little below the text.
        private static TextDecorationCollection Underline()
        {
            TextDecoration d = new TextDecoration(TextDecorationLocation.Underline, new Pen(Muted, 1), 3, TextDecorationUnit.Pixel, TextDecorationUnit.Pixel);
            TextDecorationCollection c = new TextDecorationCollection();
            c.Add(d);
            c.Freeze();
            return c;
        }

        private static bool InLink(DependencyObject d)
        {
            for (; d != null; d = d is FrameworkContentElement ? ((FrameworkContentElement)d).Parent : null) {
                if (d is Hyperlink) {
                    return true;
                }
            }
            return false;
        }

        private void ShowPath()
        {
            m_path.Text = m_installDir.TrimEnd('\\');
            m_path.ToolTip = m_path.Text;
        }

        private void ShowAgree()
        {
            m_box.Background = m_agreed ? Brushes.White : Brushes.Transparent;
            m_box.BorderBrush = m_agreed ? Brushes.White : Muted;
            m_tick.Visibility = m_agreed ? Visibility.Visible : Visibility.Hidden;
            m_agree.Inlines.FirstInline.Foreground = m_agreed ? Brushes.White : Muted;
            m_button.Active = m_agreed;
        }

        private void ToggleAgree()
        {
            if (m_installing) {
                return;
            }
            m_agreed = !m_agreed;
            ShowAgree();
        }

        private void OnKey(object sender, KeyEventArgs e)
        {
            if (e.Key == Key.Escape && !m_installing) {
                Close();
            } else if (e.Key == Key.Space && m_box.Visibility == Visibility.Visible) {
                ToggleAgree();
                e.Handled = true;
            } else if (e.Key == Key.Enter) {
                m_button.Fire();
                e.Handled = true;
            }
        }

        private void OnClosing(object sender, CancelEventArgs e)
        {
            if (m_installing) {
                e.Cancel = true; // Windows Installer is mid-way: let it finish
            }
        }

        // --- resources and paths ---------------------------------------------------------------------

        private static Stream Resource(string name)
        {
            Stream s = Assembly.GetExecutingAssembly().GetManifestResourceStream(name);
            if (s == null) {
                throw new FileNotFoundException("missing resource " + name);
            }
            return s;
        }

        private static string ReadText(string name)
        {
            using (StreamReader r = new StreamReader(Resource(name))) {
                return r.ReadToEnd();
            }
        }

        private static void Extract(string name, string to)
        {
            using (Stream s = Resource(name)) {
                using (FileStream f = File.Create(to)) {
                    s.CopyTo(f);
                }
            }
        }

        //! Where an installed Ploq lives (so an upgrade stays put), else %LOCALAPPDATA%\Programs\Ploq\.
        private static string DefaultInstallDir()
        {
            try {
                using (RegistryKey k = Registry.CurrentUser.OpenSubKey(@"Software\Ploq\Installer")) {
                    string v = k != null ? k.GetValue("InstallDir") as string : null;
                    if (!string.IsNullOrEmpty(v)) {
                        return v.EndsWith("\\") ? v : v + "\\";
                    }
                }
            } catch (Exception) {
                // fall through to the default
            }
            return System.IO.Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Programs", "Ploq") + "\\";
        }

        private void ChooseFolder()
        {
            if (m_installing) {
                return;
            }
            using (System.Windows.Forms.FolderBrowserDialog d = new System.Windows.Forms.FolderBrowserDialog()) {
                d.Description = "Choose where to install Ploq. A Ploq folder is made inside it.";
                d.SelectedPath = System.IO.Path.GetDirectoryName(m_installDir.TrimEnd('\\'));
                if (d.ShowDialog() != System.Windows.Forms.DialogResult.OK || string.IsNullOrEmpty(d.SelectedPath)) {
                    return;
                }
                string p = d.SelectedPath.TrimEnd('\\');
                if (!p.EndsWith("\\Ploq", StringComparison.OrdinalIgnoreCase)) {
                    p = System.IO.Path.Combine(p, "Ploq");
                }
                m_installDir = p + "\\";
                ShowPath();
            }
        }

        private static void Open(string target)
        {
            try {
                Process.Start(new ProcessStartInfo(target) { UseShellExecute = true });
            } catch (Exception) {
                // nothing to do: the link just doesn't open
            }
        }

        private void Cleanup()
        {
            try {
                Directory.Delete(m_work, true);
            } catch (Exception) {
                // fonts in use or already gone: Windows cleans %TEMP% eventually
            }
        }

        // --- install ---------------------------------------------------------------------------------

        private void SetStatus(string text)
        {
            m_status.Text = text;
            m_status.Visibility = Visibility.Visible;
            m_box.Visibility = Visibility.Collapsed;
            m_agree.Visibility = Visibility.Collapsed;
        }

        private void StartInstall()
        {
            if (!m_agreed || m_installing) {
                return;
            }
            if (Process.GetProcessesByName("Ploq").Length > 0) {
                SetStatus("Ploq is open. Close it, then click Install.");
                m_box.Visibility = Visibility.Collapsed;
                m_buttonAction = StartInstall;
                return;
            }
            m_installing = true;
            m_close.Foreground = Dim;
            m_change.Visibility = Visibility.Collapsed;
            m_button.Active = false;
            m_button.Text = "Installing";
            SetStatus("Installing Ploq " + m_version + "…");
            m_bar.Width = 0;

            string msi = System.IO.Path.Combine(m_work, "Ploq-" + m_version + ".msi");
            string log = System.IO.Path.Combine(System.IO.Path.GetTempPath(), "Ploq-setup.log");
            string dir = m_installDir;
            Thread t = new Thread(delegate() {
                uint code;
                string problem = null;
                try {
                    Extract("Ploq.msi", msi);
                    code = RunMsi(msi, dir, log, out problem);
                } catch (Exception ex) {
                    code = 1603;
                    problem = ex.Message;
                }
                Dispatcher.BeginInvoke(new Action(delegate { Finished(code, problem, log); }));
            });
            t.IsBackground = true;
            t.Start();
        }

        // Progress from Windows Installer's progress messages (the documented algorithm: reset, action
        // info, increment, add to total). Called on the install thread.
        private long m_total, m_done;
        private bool m_forward = true;
        private double m_shown;
        private Action<int> m_onPercent; // --install-quiet only
        private static Msi.UiHandler s_handler; // kept alive while Windows Installer holds it

        private uint RunMsi(string msi, string dir, string log, out string problem)
        {
            string lastError = null;
            Msi.MsiEnableLog(Msi.INSTALLLOGMODE_ERROR | Msi.INSTALLLOGMODE_WARNING | Msi.INSTALLLOGMODE_VERBOSE, log, 0);
            Msi.MsiSetInternalUI(Msi.INSTALLUILEVEL_NONE, IntPtr.Zero);
            s_handler = delegate(IntPtr ctx, uint type, string message) {
                uint kind = type & 0xFF000000;
                if (kind == Msi.INSTALLMESSAGE_PROGRESS) {
                    OnProgress(message);
                } else if (kind == Msi.INSTALLMESSAGE_ERROR && !string.IsNullOrEmpty(message)) {
                    lastError = message;
                }
                return 0;
            };
            Msi.MsiSetExternalUI(s_handler, Msi.INSTALLLOGMODE_PROGRESS | Msi.INSTALLLOGMODE_ERROR | Msi.INSTALLLOGMODE_WARNING, IntPtr.Zero);
            // INSTALLDIR: the .msi's install folder (Ploq.wxs). PLOQ_RELAUNCH=0: we open Ploq ourselves.
            string args = "INSTALLDIR=\"" + dir + "\" PLOQ_RELAUNCH=0 REBOOT=ReallySuppress";
            uint code = Msi.MsiInstallProduct(msi, args);
            problem = lastError;
            return code;
        }

        private void OnProgress(string message)
        {
            if (string.IsNullOrEmpty(message)) {
                return;
            }
            // "1: 0 2: 1234 3: 0 4: 1" -> field number -> value
            Dictionary<int, long> f = new Dictionary<int, long>();
            string[] tokens = message.Split(new[] { ' ' }, StringSplitOptions.RemoveEmptyEntries);
            for (int i = 0; i + 1 < tokens.Length; i += 2) {
                int key;
                long value;
                if (int.TryParse(tokens[i].TrimEnd(':'), out key) && long.TryParse(tokens[i + 1], out value)) {
                    f[key] = value;
                }
            }
            long type;
            if (!f.TryGetValue(1, out type)) {
                return;
            }
            long v2 = f.ContainsKey(2) ? f[2] : 0;
            long v3 = f.ContainsKey(3) ? f[3] : 0;
            switch (type) {
            case 0: // reset: new total, direction
                m_total = v2;
                m_forward = v3 == 0;
                m_done = m_forward ? 0 : m_total;
                break;
            case 2: // increment
                m_done += m_forward ? v2 : -v2;
                break;
            case 3: // add to total
                m_total += v2;
                break;
            default:
                return;
            }
            if (m_total <= 0) {
                return;
            }
            double p = Math.Max(0, Math.Min(1, (double)m_done / m_total));
            if (p < m_shown) {
                return; // a new phase starts at 0: don't let the bar jump back
            }
            m_shown = p;
            if (m_onPercent != null) {
                m_onPercent((int)Math.Round(p * 100));
                return;
            }
            Dispatcher.BeginInvoke(new Action(delegate {
                m_bar.Width = (W - 2 * PAD) * p;
                m_status.Text = "Installing Ploq " + m_version + "… " + (int)Math.Round(p * 100) + "%";
            }));
        }

        private void Finished(uint code, string problem, string log)
        {
            m_installing = false;
            m_close.Foreground = Muted;
            if (code == 0 || code == 3010) {
                m_bar.Width = W - 2 * PAD;
                SetStatus("Ploq " + m_version + " is installed.");
                m_installTo.Visibility = Visibility.Collapsed;
                m_path.Visibility = Visibility.Collapsed;
                m_change.Visibility = Visibility.Collapsed;
                m_button.Text = "Open Ploq";
                m_button.Active = true;
                m_buttonAction = delegate {
                    Open(System.IO.Path.Combine(m_installDir, "bin", "Ploq.exe"));
                    Close();
                };
                return;
            }
            m_bar.Width = 0;
            m_change.Visibility = Visibility.Visible;
            if (code == 1602) {
                SetStatus("The installation was cancelled.");
            } else if (code == 1618) {
                SetStatus("Another installation is running. Try again in a minute.");
            } else {
                SetStatus("Ploq couldn't be installed" + (string.IsNullOrEmpty(problem) ? ". " : ": " + problem.Trim() + " "));
                Hyperlink details = new Hyperlink(new Run("See details"));
                details.Foreground = Brushes.White;
                details.TextDecorations = Underline();
                details.Cursor = Cursors.Hand;
                details.Click += delegate { Open(log); };
                m_status.Inlines.Add(details);
            }
            m_button.Text = "Try again";
            m_button.Active = true;
            m_buttonAction = StartInstall;
        }

        //! Development only: draw the window in one of its states to a PNG without showing it:
        //! PloqSetup.exe --render out.png [unticked|ticked|installing|done|error|open] [scale]
        private void RenderTo(string png, string state, double scale)
        {
            switch (state) {
            case "ticked":
                m_agreed = true;
                ShowAgree();
                break;
            case "installing":
                m_agreed = true;
                ShowAgree();
                m_change.Visibility = Visibility.Collapsed;
                m_button.Active = false;
                m_button.Text = "Installing";
                SetStatus("Installing Ploq " + m_version + "… 62%");
                m_bar.Width = (W - 2 * PAD) * 0.62;
                break;
            case "done":
                Finished(0, null, "");
                break;
            case "error":
                m_agreed = true;
                ShowAgree();
                Finished(1603, "There is not enough space on the disk.", System.IO.Path.Combine(System.IO.Path.GetTempPath(), "Ploq-setup.log"));
                break;
            case "open":
                m_agreed = true;
                ShowAgree();
                SetStatus("Ploq is open. Close it, then click Install.");
                break;
            }
            Content = null;
            Border frame = new Border();
            frame.Background = Brushes.Black;
            frame.BorderBrush = Line;
            frame.BorderThickness = new Thickness(1);
            frame.Child = m_root;
            frame.Measure(new Size(W, H));
            frame.Arrange(new Rect(0, 0, W, H));
            frame.UpdateLayout();
            RenderTargetBitmap bmp = new RenderTargetBitmap((int)(W * scale), (int)(H * scale), 96 * scale, 96 * scale, PixelFormats.Pbgra32);
            bmp.Render(frame);
            PngBitmapEncoder enc = new PngBitmapEncoder();
            enc.Frames.Add(BitmapFrame.Create(bmp));
            using (FileStream f = File.Create(png)) {
                enc.Save(f);
            }
        }

        [STAThread]
        public static void Main(string[] args)
        {
            if (args.Length >= 2 && args[0] == "--install-quiet") {
                // Development only: the real install (same code as the Install button) with no window;
                // writes the progress it saw and Windows Installer's result to args[1].
                Setup w = new Setup();
                string msi = System.IO.Path.Combine(w.m_work, "Ploq-" + w.m_version + ".msi");
                Extract("Ploq.msi", msi);
                List<string> seen = new List<string>();
                w.m_onPercent = delegate(int p) { lock (seen) { if (seen.Count == 0 || seen[seen.Count - 1] != p.ToString()) seen.Add(p.ToString()); } };
                string problem;
                uint code = w.RunMsi(msi, w.m_installDir, System.IO.Path.Combine(System.IO.Path.GetTempPath(), "Ploq-setup.log"), out problem);
                File.WriteAllText(args[1], "code " + code + "\ndir " + w.m_installDir + "\nproblem " + problem + "\nprogress " + string.Join(" ", seen.ToArray()) + "\n");
                w.Cleanup();
                return;
            }
            if (args.Length >= 2 && args[0] == "--render") {
                Setup w = new Setup();
                double scale = args.Length > 3 ? double.Parse(args[3], System.Globalization.CultureInfo.InvariantCulture) : 1;
                w.RenderTo(args[1], args.Length > 2 ? args[2] : "unticked", scale);
                w.Cleanup();
                return;
            }
            Application app = new Application();
            app.ShutdownMode = ShutdownMode.OnMainWindowClose;
            app.Run(new Setup());
        }
    }
}

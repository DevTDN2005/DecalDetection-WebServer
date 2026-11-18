using System;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.IO.Ports;
using System.Net.Sockets;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace app
{
    public partial class Form1 : Form
    {
        // ============================================================
        // UART
        // ============================================================
        private bool isUartConnected = false;
        private Task uartTask;
        private volatile bool uartStop = false;

        private readonly object uartWriteLock = new object();
        private DateTime lastPongUtc = DateTime.MinValue;

        // Chi log retry moi 2 giay de de debug ma khong spam qua nhieu.
        private DateTime lastUartRetryLogUtc = DateTime.MinValue;

        // Chi xoa log 1 lan khi ESP32 ket noi thanh cong lan dau.
        private bool uartConnectedLogCleared = false;

        // ============================================================
        // CAMERA STREAM FROM System
        // ============================================================
        private TcpClient cameraClient;
        private volatile bool cameraReceiverStop = false;
        private Task cameraReceiverTask;
        private int cameraUiPending = 0;

        // Reuse one receive buffer. Repeated new byte[jpegSize] allocations
        // (>85 KB) put pressure on Mono's large-object heap on Jetson Nano.
        private byte[] cameraJpegBuffer = new byte[512 * 1024];

        // Preview UI chi can 10 FPS. AI ben C++ van xu ly frame goc.
        // Giam JPEG decode + Bitmap allocation + redraw + tai RealVNC.
        private readonly Stopwatch cameraPreviewClock = Stopwatch.StartNew();
        private long lastCameraPreviewMs = 0;
        private const int CAMERA_PREVIEW_INTERVAL_MS = 200; // 5 FPS UI/VNC; AI stays full speed.

        private Process aiProcess;
        private bool cameraBusy = false;

        // ADD sends a roll-management command to persistent C++ on localhost.
        private bool addBusy = false;
        private const int AI_CONTROL_PORT = 5001;

        // Dang shutdown: chan cac thread background day log/frame ve UI.
        private volatile bool appClosing = false;

        public Form1()
        {
            InitializeComponent();
        }

        private async void Form1_Load(object sender, EventArgs e)
        {
            // ========================================================
            // UART CONFIG
            // ========================================================
            serialPort.PortName = "/dev/ttyTHS1";
            serialPort.BaudRate = 115200;
            serialPort.DataBits = 8;
            serialPort.Parity = Parity.None;
            serialPort.StopBits = StopBits.One;
            serialPort.Handshake = Handshake.None;
            serialPort.DtrEnable = false;
            serialPort.RtsEnable = false;
            serialPort.NewLine = "\n";

            serialPort.ReadTimeout = 200;
            serialPort.WriteTimeout = 1000;

            UpdateUartStatus(false);

            // CLOCK
            timer1.Interval = 1000;
            timer1.Tick += Timer1_Tick;
            timer1.Start();
            UpdateClock();

            // LOG
            InfoBox.Text = "";
            AddLog("Application started.");

            // CAMERA DISPLAY
            picCamera.BackColor = Color.Black;
            picCamera.SizeMode = PictureBoxSizeMode.Zoom;

            // UART and AI are independent.
            // AI/camera start immediately; RUN button is MOTOR control only.
            StartUartAutoConnect();

            AddLog("Starting camera + AI...");
            StartCameraReceiver();
            await StartAiAsync();

            // Production state is restored by C++ directly from Firebase.
            // Do NOT send APP_START here: restarting/recompiling the App must
            // never create a new production roll. Only ADD creates the next roll.
            AddLog("Production roll state is restored from Firebase by C++.");
        }

        // ============================================================
        // UART STATUS UI
        // ============================================================
        private void UpdateUartStatus(bool connected)
        {
            isUartConnected = connected;

            if (connected)
            {
                STATECONNECT.Text = "CONNECTED";
                STATECONNECT.BackColor = Color.LimeGreen;
            }
            else
            {
                STATECONNECT.Text = "DISCONNECTED";
                STATECONNECT.BackColor = Color.Salmon;
            }

            STATECONNECT.TabStop = false;
        }

        // ============================================================
        // UART SEND
        // ============================================================
        private void UartWriteLine(string command)
        {
            lock (uartWriteLock)
            {
                if (!serialPort.IsOpen)
                    throw new InvalidOperationException("UART is not open.");

                serialPort.WriteLine(command);
            }
        }

        private void SendMotorCommand(string command)
        {
            if (!isUartConnected || !serialPort.IsOpen)
            {
                AddLog("Cannot " + command + ": ESP32 disconnected.");
                return;
            }

            try
            {
                UartWriteLine(command);

                if (command == "RUN")
                    AddLog("RUN pressed (App).");
                else if (command == "STOP")
                    AddLog("STOP pressed (App).");
            }
            catch (Exception ex)
            {
                AddLog("UART send error: " + ex.Message);
            }
        }

        // ============================================================
        // UART AUTO CONNECT + RECEIVE
        // ============================================================
        private void StartUartAutoConnect()
        {
            if (uartTask != null && !uartTask.IsCompleted)
                return;

            uartStop = false;

            uartTask = Task.Run(() =>
            {
                DateTime lastPingUtc = DateTime.MinValue;

                while (!uartStop)
                {
                    try
                    {
                        if (!serialPort.IsOpen)
                        {
                            UartLogThrottled(
                                "Opening UART " + serialPort.PortName + "..."
                            );

                            serialPort.Open();
                            serialPort.DiscardInBuffer();
                            serialPort.DiscardOutBuffer();

                            lastPingUtc = DateTime.MinValue;
                            lastPongUtc = DateTime.MinValue;

                            UartLog(
                                "UART opened: " + serialPort.PortName
                            );
                        }

                        DateTime now = DateTime.UtcNow;

                        if ((now - lastPingUtc).TotalMilliseconds >= 1000)
                        {
                            UartWriteLine("PING");
                            lastPingUtc = now;

                            if (!isUartConnected)
                                UartLog("TX -> ESP32: PING");
                        }

                        try
                        {
                            string line = serialPort.ReadLine().Trim();

                            if (line.Length > 0)
                                ProcessUartLine(line);
                        }
                        catch (TimeoutException)
                        {
                            if (!isUartConnected)
                            {
                                UartLogThrottled(
                                    "Waiting for ESP32 PONG..."
                                );
                            }
                        }

                        if (isUartConnected &&
                            lastPongUtc != DateTime.MinValue &&
                            (DateTime.UtcNow - lastPongUtc).TotalMilliseconds > 3000)
                        {
                            throw new TimeoutException(
                                "ESP32 heartbeat timeout."
                            );
                        }
                    }
                    catch (Exception ex)
                    {
                        HandleUartDisconnect(ex.Message);
                    }
                }
            });
        }

        private void UartLogThrottled(string message)
        {
            DateTime now = DateTime.UtcNow;

            if ((now - lastUartRetryLogUtc).TotalMilliseconds < 2000)
                return;

            lastUartRetryLogUtc = now;
            UartLog(message);
        }

        private void ProcessUartLine(string line)
        {
            if (line == "PONG")
            {
                lastPongUtc = DateTime.UtcNow;

                if (!isUartConnected)
                {
                    UpdateUartStatusSafe(true);

                    ClearLogAfterUartConnectedSafe();

                    // Camera + AI are already running independently.
                    // PONG only confirms the ESP32/UART side.
                }

                return;
            }

            if (line.StartsWith("STATE:"))
            {
                bool isRun = line.StartsWith("STATE:RUN");
                bool isStop = line.StartsWith("STATE:STOP");
                bool fromButton = line.Contains("SOURCE:BUTTON");
                bool fromApp = line.Contains("SOURCE:APP");

                if (isRun && fromButton)
                    UartLog("RUN pressed (Physical button).");
                else if (isStop && fromButton)
                    UartLog("STOP pressed (Physical button).");
                else if (isRun && fromApp)
                    UartLog("RUN confirmed by ESP32.");
                else if (isStop && fromApp)
                    UartLog("STOP confirmed by ESP32.");

                // AI + background web sync are ALWAYS ON after warm-up.
                // RUN/STOP only control the conveyor motor. Defect images and
                // totals are already being synchronized by the persistent C++ worker.
                return;
            }
        }

        private void ClearLogAfterUartConnectedSafe()
        {
            if (uartConnectedLogCleared)
                return;

            uartConnectedLogCleared = true;

            if (appClosing || IsDisposed || Disposing || !IsHandleCreated)
                return;

            try
            {
                BeginInvoke(new Action(() =>
                {
                    if (IsDisposed || Disposing)
                        return;

                    InfoBox.Text = "";
                    AddLog("ESP32 connected.");
                }));
            }
            catch (InvalidOperationException)
            {
            }
        }

        private void HandleUartDisconnect(string reason)
        {
            bool wasConnected = isUartConnected;

            isUartConnected = false;
            UpdateUartStatusSafe(false);

            try
            {
                if (serialPort.IsOpen)
                    serialPort.Close();
            }
            catch
            {
            }

            if (wasConnected)
            {
                UartLog("ESP32 disconnected: " + reason);
            }
            else
            {
                UartLogThrottled(
                    "UART connect failed: " + reason
                );
            }

            if (!uartStop)
                Thread.Sleep(1000);
        }

        private void UpdateUartStatusSafe(bool connected)
        {
            if (IsDisposed || Disposing || !IsHandleCreated)
                return;

            try
            {
                BeginInvoke(new Action(() =>
                {
                    if (!IsDisposed && !Disposing)
                        UpdateUartStatus(connected);
                }));
            }
            catch (InvalidOperationException)
            {
            }
        }

        private void UartLog(string message)
        {
            if (IsDisposed || Disposing || !IsHandleCreated)
                return;

            try
            {
                BeginInvoke(new Action(() =>
                {
                    if (!IsDisposed && !Disposing)
                        AddLog(message);
                }));
            }
            catch (InvalidOperationException)
            {
            }
        }

        // ============================================================
        // CAMERA + AI START IMMEDIATELY; UART IS INDEPENDENT
        // ============================================================
        // CLOCK
        // ============================================================
        private void Timer1_Tick(object sender, EventArgs e)
        {
            UpdateClock();
        }

        private void UpdateClock()
        {
            RealtimeBox.Text = DateTime.Now.ToString("HH:mm:ss");
        }

        // ============================================================
        // LOG
        // ============================================================
        private void AddLog(string message)
        {
            string line =
                "[" + DateTime.Now.ToString("HH:mm:ss") + "] "
                + message + Environment.NewLine;

            InfoBox.AppendText(line);
            if (InfoBox.TextLength > 12000)
            {
                int remove = InfoBox.TextLength - 9000;
                InfoBox.Select(0, remove);
                InfoBox.SelectedText = "";
            }

            InfoBox.SelectionStart = InfoBox.Text.Length;
            InfoBox.ScrollToCaret();
        }

        // ============================================================
        // CAMERA TCP RECEIVER
        // ============================================================
        private bool ReadExact(NetworkStream stream, byte[] buffer, int length)
        {
            int offset = 0;

            while (offset < length)
            {
                int received = stream.Read(buffer, offset, length - offset);

                if (received <= 0)
                    return false;

                offset += received;
            }

            return true;
        }

        private void StartCameraReceiver()
        {
            if (cameraReceiverTask != null && !cameraReceiverTask.IsCompleted)
                return;

            cameraReceiverStop = false;

            cameraReceiverTask = Task.Run(() =>
            {
                CameraReceiveLoop();
            });
        }

        private void CameraReceiveLoop()
        {
            byte[] header = new byte[4];
            bool announcedWaiting = false;

            while (!cameraReceiverStop)
            {
                TcpClient client = null;

                try
                {
                    client = new TcpClient();
                    client.NoDelay = true;

                    cameraClient = client;
                    client.Connect("127.0.0.1", 5000);

                    announcedWaiting = false;
                    CameraLog("Camera activated.");

                    using (NetworkStream stream = client.GetStream())
                    {
                        while (!cameraReceiverStop)
                        {
                            if (!ReadExact(stream, header, 4))
                                break;

                            int jpegSize =
                                (header[0] << 24) |
                                (header[1] << 16) |
                                (header[2] << 8) |
                                header[3];

                            if (jpegSize <= 0 || jpegSize > 5 * 1024 * 1024)
                                throw new Exception(
                                    "Invalid camera frame size: " + jpegSize
                                );

                            if (jpegSize > cameraJpegBuffer.Length)
                            {
                                int newSize = cameraJpegBuffer.Length;

                                while (newSize < jpegSize)
                                    newSize *= 2;

                                cameraJpegBuffer = new byte[newSize];

                                CameraLog(
                                    "Camera buffer resized to "
                                    + (newSize / 1024)
                                    + " KB."
                                );
                            }

                            if (!ReadExact(
                                    stream,
                                    cameraJpegBuffer,
                                    jpegSize))
                            {
                                break;
                            }

                            long nowPreviewMs = cameraPreviewClock.ElapsedMilliseconds;

                            if (Volatile.Read(ref cameraUiPending) == 1 ||
                                nowPreviewMs - lastCameraPreviewMs <
                                    CAMERA_PREVIEW_INTERVAL_MS)
                            {
                                continue;
                            }

                            lastCameraPreviewMs = nowPreviewMs;

                            Bitmap bitmap;

                            using (MemoryStream ms =
                                new MemoryStream(
                                    cameraJpegBuffer,
                                    0,
                                    jpegSize,
                                    false,
                                    true))
                            using (Image image = Image.FromStream(ms))
                            {
                                bitmap = new Bitmap(image);
                            }

                            if (Interlocked.Exchange(ref cameraUiPending, 1) == 1)
                            {
                                bitmap.Dispose();
                                continue;
                            }

                            if (IsDisposed || Disposing || !IsHandleCreated)
                            {
                                bitmap.Dispose();
                                Interlocked.Exchange(ref cameraUiPending, 0);
                                continue;
                            }

                            try
                            {
                                BeginInvoke(new Action(() =>
                                {
                                    try
                                    {
                                        Image oldImage = picCamera.Image;
                                        picCamera.Image = bitmap;

                                        if (oldImage != null)
                                            oldImage.Dispose();
                                    }
                                    finally
                                    {
                                        Interlocked.Exchange(
                                            ref cameraUiPending,
                                            0
                                        );
                                    }
                                }));
                            }
                            catch (InvalidOperationException)
                            {
                                bitmap.Dispose();
                                Interlocked.Exchange(
                                    ref cameraUiPending,
                                    0
                                );
                            }
                        }
                    }
                }
                catch (Exception ex)
                {
                    if (!cameraReceiverStop && !announcedWaiting)
                    {
                        CameraLog(
                            "Waiting for System camera stream: "
                            + ex.Message
                        );

                        announcedWaiting = true;
                    }
                }
                finally
                {
                    try
                    {
                        if (client != null)
                            client.Close();
                    }
                    catch
                    {
                    }

                    if (ReferenceEquals(cameraClient, client))
                        cameraClient = null;
                }

                if (!cameraReceiverStop)
                    Thread.Sleep(500);
            }
        }

        private void StopCameraReceiver()
        {
            cameraReceiverStop = true;

            try
            {
                if (cameraClient != null)
                    cameraClient.Close();
            }
            catch
            {
            }

            cameraClient = null;

            if (picCamera.Image != null)
            {
                Image oldImage = picCamera.Image;
                picCamera.Image = null;
                oldImage.Dispose();
            }
        }

        // ============================================================
        // START SYSTEM
        // ============================================================
        private async Task StartAiAsync()
        {
            if (cameraBusy)
                return;

            if (Environment.OSVersion.Platform != PlatformID.Unix)
            {
                AddLog("Camera command is available on Jetson only.");
                return;
            }
            if (IsMainProcessRunning())
            {
                AddLog("AI models already loaded. Reusing running AI.");
                return;
            }

            cameraBusy = true;

            try
            {
                Process process = new Process();

                process.StartInfo = new ProcessStartInfo
                {
                    FileName = "/usr/bin/sudo",
                    Arguments = "-n /home/jetson/TDN/Main/main",
                    WorkingDirectory = "/home/jetson/TDN/Main",
                    UseShellExecute = false,
                    RedirectStandardOutput = false,
                    RedirectStandardError = false,
                    CreateNoWindow = true
                };

                process.Start();

                aiProcess = process;

                AddLog("Starting persistent AI process...");

                await Task.Delay(1000);

                if (process.HasExited)
                {
                    AddLog("AI startup failed. Code: " + process.ExitCode);

                    process.Dispose();
                    aiProcess = null;
                }
                else
                {
                    AddLog("AI process running. Models remain loaded.");
                }
            }
            catch (Exception ex)
            {
                AddLog("Start AI failed: " + ex.Message);
            }
            finally
            {
                cameraBusy = false;
            }
        }

        // ============================================================
        // STOP SYSTEM KHI DONG APP
        // ============================================================
        private bool IsMainProcessRunning()
        {
            if (Environment.OSVersion.Platform != PlatformID.Unix)
                return false;

            try
            {
                using (Process command = new Process())
                {
                    command.StartInfo = new ProcessStartInfo
                    {
                        FileName = "/usr/bin/pgrep",
                        Arguments = "-x main",
                        UseShellExecute = false,
                        RedirectStandardOutput = true,
                        RedirectStandardError = true,
                        CreateNoWindow = true
                    };

                    command.Start();
                    command.WaitForExit(500);

                    return command.ExitCode == 0;
                }
            }
            catch
            {
                return false;
            }
        }

        // ============================================================
        // AI PROCESS PERSISTENCE
        // App dong KHONG kill C++ main.
        // Hai TensorRT engines tiep tuc nam san trong process main.
        // ============================================================
        private void DetachAiProcessForApplicationExit()
        {
            try
            {
                // Chi giai phong Process handle cua WinForms.
                // KHONG gui SIGINT / SIGTERM / SIGKILL.
                if (aiProcess != null)
                {
                    aiProcess.Dispose();
                    aiProcess = null;
                }
            }
            catch
            {
                aiProcess = null;
            }
        }

        // ============================================================
        // LOG CAMERA/AI -> UI
        // ============================================================
        private void CameraLog(string message)
        {
            if (appClosing || IsDisposed || Disposing || !IsHandleCreated)
                return;

            try
            {
                BeginInvoke(new Action(() =>
                {
                    if (!IsDisposed && !Disposing)
                        AddLog(message);
                }));
            }
            catch (InvalidOperationException)
            {
            }
        }

        // ============================================================
        // FORM CLOSING - CLEAN SHUTDOWN
        // Override truc tiep de chac chan luon duoc goi khi this.Close(),
        // nut X, Alt+F4 hoac he thong dong Form.
        // ============================================================
        protected override void OnFormClosing(
            FormClosingEventArgs e)
        {
            if (appClosing)
            {
                base.OnFormClosing(e);
                return;
            }

            appClosing = true;

            // Stop only the physical conveyor. C++ AI + upload workers are
            // persistent and continue finishing any queued GitHub/Firebase work.
            try
            {
                if (isUartConnected && serialPort.IsOpen)
                    UartWriteLine("STOP");
            }
            catch
            {
            }

            // IMPORTANT:
            // Closing/restarting the WinForms UI must NOT close the production roll.
            // C++ keeps the current roll in Firebase and will resume it on restart.
            // Only ADD is allowed to close the current roll and create the next one.

            // Stop heartbeat / UART task.
            uartStop = true;

            try
            {
                if (serialPort.IsOpen)
                    serialPort.Close();
            }
            catch
            {
            }

            try
            {
                if (uartTask != null && !uartTask.IsCompleted)
                    uartTask.Wait(500);
            }
            catch
            {
            }

            // Stop only the WinForms preview receiver.
            StopCameraReceiver();

            try
            {
                if (cameraReceiverTask != null &&
                    !cameraReceiverTask.IsCompleted)
                {
                    cameraReceiverTask.Wait(500);
                }
            }
            catch
            {
            }

            // IMPORTANT: do not kill /home/jetson/TDN/Main/main.
            // Both TensorRT engines and the background server-sync worker stay alive.
            DetachAiProcessForApplicationExit();

            base.OnFormClosing(e);
        }


        // Compatibility shim:
        // Neu Designer cu van dang hook event FormClosing vao ham nay
        // thi giu ham rong de project van compile. Cleanup that nam trong
        // override OnFormClosing() phia tren.
        private void Form1_OnFormClosing(
            object sender,
            FormClosingEventArgs e)
        {
        }

        // ============================================================
        // AI / PRODUCTION CONTROL
        //
        // Camera stream: C++ -> App on TCP 5000.
        // Production command: App -> C++ on TCP 5001.
        // Normal RUN/STOP are UART motor commands only.
        // C++ continuously detects + uploads in the background.
        // Production lifecycle:
        //   C++ startup   -> restore current active roll directly from Firebase
        //   ADD           -> close current roll + create next roll
        //   App restart   -> must NOT create/close a roll.
        // ============================================================
        private string SendAiControlCommand(
            string command,
            int timeoutMs)
        {
            using (TcpClient client = new TcpClient())
            {
                client.ReceiveTimeout = timeoutMs;
                client.SendTimeout = 3000;

                client.Connect(
                    "127.0.0.1",
                    AI_CONTROL_PORT
                );

                using (NetworkStream stream = client.GetStream())
                using (StreamWriter writer = new StreamWriter(stream))
                using (StreamReader reader = new StreamReader(stream))
                {
                    writer.NewLine = "\n";
                    writer.AutoFlush = true;
                    writer.WriteLine(command);

                    string response = reader.ReadLine();

                    return string.IsNullOrWhiteSpace(response)
                        ? "ERR EMPTY_RESPONSE"
                        : response.Trim();
                }
            }
        }

        private async Task<string> SendAiControlCommandAsync(
            string command,
            int timeoutMs = 120000)
        {
            return await Task.Run(() =>
                SendAiControlCommand(command, timeoutMs)
            );
        }

        // Production session is restored by C++ from Firebase.
        // No APP_START command is needed here.


        // ============================================================
        // CONTROL BUTTONS
        // ============================================================
        private void StopMotor_Click(object sender, EventArgs e)
        {
            SendMotorCommand("STOP");
        }

        private void RunMotor_Click(object sender, EventArgs e)
        {
            SendMotorCommand("RUN");
        }

        private async void addDecal_Click(object sender, EventArgs e)
        {
            if (addBusy)
                return;

            if (!IsMainProcessRunning())
            {
                AddLog(
                    "Cannot ADD: AI process is not running."
                );
                return;
            }

            addBusy = true;
            addDecal.Enabled = false;

            AddLog(
                "ADD pressed. Finalizing current roll locally..."
            );

            try
            {
                string response =
                    await SendAiControlCommandAsync(
                        "ADD",
                        120000
                    );

                if (response.StartsWith(
                        "OK NEW_ROLL ",
                        StringComparison.Ordinal))
                {
                    string[] parts =
                        response.Split(
                            new[] { ' ' },
                            StringSplitOptions.RemoveEmptyEntries
                        );

                    if (parts.Length >= 3)
                    {
                        AddLog(
                            "New roll created: Roll "
                            + parts[2]
                            + "."
                        );
                    }
                    else
                    {
                        AddLog(
                            "New roll created."
                        );
                    }
                }
                else
                {
                    AddLog(
                        "ADD result: "
                        + response
                    );
                }
            }
            catch (Exception ex)
            {
                AddLog(
                    "ADD failed: "
                    + ex.Message
                );
            }
            finally
            {
                addBusy = false;

                if (!appClosing)
                    addDecal.Enabled = true;
            }
        }


        private void EXIT_Click(object sender, EventArgs e)
        {
            this.Close();
        }
    }
}

namespace app
{
    partial class Form1
    {
        /// <summary>
        /// Required designer variable.
        /// </summary>
        private System.ComponentModel.IContainer components = null;

        /// <summary>
        /// Clean up any resources being used.
        /// </summary>
        /// <param name="disposing">true if managed resources should be disposed; otherwise, false.</param>
        protected override void Dispose(bool disposing)
        {
            if (disposing && (components != null))
            {
                components.Dispose();
            }
            base.Dispose(disposing);
        }

        #region Windows Form Designer generated code

        /// <summary>
        /// Required method for Designer support - do not modify
        /// the contents of this method with the code editor.
        /// </summary>
        private void InitializeComponent()
        {
            this.components = new System.ComponentModel.Container();
            this.serialPort = new System.IO.Ports.SerialPort(this.components);
            this.timer1 = new System.Windows.Forms.Timer(this.components);
            this.STATECONNECT = new System.Windows.Forms.Button();
            this.UARTTITLEBOX = new System.Windows.Forms.TextBox();
            this.uartPanel = new System.Windows.Forms.GroupBox();
            this.InfoBox = new System.Windows.Forms.TextBox();
            this.logPanel = new System.Windows.Forms.GroupBox();
            this.Title = new System.Windows.Forms.TextBox();
            this.RealtimeBox = new System.Windows.Forms.TextBox();
            this.controlPanel = new System.Windows.Forms.GroupBox();
            this.addDecal = new System.Windows.Forms.Button();
            this.StopMotor = new System.Windows.Forms.Button();
            this.RunMotor = new System.Windows.Forms.Button();
            this.cameraPanel = new System.Windows.Forms.GroupBox();
            this.picCamera = new System.Windows.Forms.PictureBox();
            this.EXIT = new System.Windows.Forms.Button();
            this.uartPanel.SuspendLayout();
            this.logPanel.SuspendLayout();
            this.controlPanel.SuspendLayout();
            this.cameraPanel.SuspendLayout();
            ((System.ComponentModel.ISupportInitialize)(this.picCamera)).BeginInit();
            this.SuspendLayout();
            // 
            // serialPort
            // 
            this.serialPort.BaudRate = 115200;
            // 
            // STATECONNECT
            // 
            this.STATECONNECT.BackColor = System.Drawing.Color.Salmon;
            this.STATECONNECT.Font = new System.Drawing.Font("Microsoft Sans Serif", 7.8F, System.Drawing.FontStyle.Bold, System.Drawing.GraphicsUnit.Point, ((byte)(163)));
            this.STATECONNECT.ForeColor = System.Drawing.Color.Maroon;
            this.STATECONNECT.Location = new System.Drawing.Point(13, 40);
            this.STATECONNECT.Margin = new System.Windows.Forms.Padding(3, 2, 3, 2);
            this.STATECONNECT.Name = "STATECONNECT";
            this.STATECONNECT.Size = new System.Drawing.Size(251, 51);
            this.STATECONNECT.TabIndex = 1;
            this.STATECONNECT.Text = "DISCONNECTED";
            this.STATECONNECT.UseVisualStyleBackColor = false;
            // 
            // UARTTITLEBOX
            // 
            this.UARTTITLEBOX.BackColor = System.Drawing.SystemColors.ButtonFace;
            this.UARTTITLEBOX.Enabled = false;
            this.UARTTITLEBOX.Font = new System.Drawing.Font("Microsoft Sans Serif", 10.2F, System.Drawing.FontStyle.Bold, System.Drawing.GraphicsUnit.Point, ((byte)(163)));
            this.UARTTITLEBOX.ForeColor = System.Drawing.Color.DarkRed;
            this.UARTTITLEBOX.Location = new System.Drawing.Point(48, 9);
            this.UARTTITLEBOX.Margin = new System.Windows.Forms.Padding(3, 2, 3, 2);
            this.UARTTITLEBOX.Name = "UARTTITLEBOX";
            this.UARTTITLEBOX.ReadOnly = true;
            this.UARTTITLEBOX.Size = new System.Drawing.Size(181, 27);
            this.UARTTITLEBOX.TabIndex = 5;
            this.UARTTITLEBOX.Text = "UART STATUS";
            this.UARTTITLEBOX.TextAlign = System.Windows.Forms.HorizontalAlignment.Center;
            // 
            // uartPanel
            // 
            this.uartPanel.Anchor = ((System.Windows.Forms.AnchorStyles)(((System.Windows.Forms.AnchorStyles.Top | System.Windows.Forms.AnchorStyles.Left)
            | System.Windows.Forms.AnchorStyles.Right)));
            this.uartPanel.BackColor = System.Drawing.SystemColors.GradientInactiveCaption;
            this.uartPanel.Controls.Add(this.UARTTITLEBOX);
            this.uartPanel.Controls.Add(this.STATECONNECT);
            this.uartPanel.Location = new System.Drawing.Point(7, 43);
            this.uartPanel.Margin = new System.Windows.Forms.Padding(3, 2, 3, 2);
            this.uartPanel.Name = "uartPanel";
            this.uartPanel.Padding = new System.Windows.Forms.Padding(3, 2, 3, 2);
            this.uartPanel.Size = new System.Drawing.Size(281, 100);
            this.uartPanel.TabIndex = 0;
            this.uartPanel.TabStop = false;
            // 
            // InfoBox
            // 
            this.InfoBox.Anchor = ((System.Windows.Forms.AnchorStyles)((((System.Windows.Forms.AnchorStyles.Top | System.Windows.Forms.AnchorStyles.Bottom)
            | System.Windows.Forms.AnchorStyles.Left)
            | System.Windows.Forms.AnchorStyles.Right)));
            this.InfoBox.BackColor = System.Drawing.SystemColors.ButtonFace;
            this.InfoBox.Font = new System.Drawing.Font("Microsoft Sans Serif", 7.8F, System.Drawing.FontStyle.Bold, System.Drawing.GraphicsUnit.Point, ((byte)(163)));
            this.InfoBox.ForeColor = System.Drawing.Color.DarkRed;
            this.InfoBox.Location = new System.Drawing.Point(7, 9);
            this.InfoBox.Margin = new System.Windows.Forms.Padding(3, 2, 3, 2);
            this.InfoBox.Multiline = true;
            this.InfoBox.Name = "InfoBox";
            this.InfoBox.ReadOnly = true;
            this.InfoBox.Size = new System.Drawing.Size(268, 244);
            this.InfoBox.TabIndex = 3;
            this.InfoBox.Text = "Log:";
            // 
            // logPanel
            // 
            this.logPanel.Anchor = ((System.Windows.Forms.AnchorStyles)(((System.Windows.Forms.AnchorStyles.Top | System.Windows.Forms.AnchorStyles.Bottom)
            | System.Windows.Forms.AnchorStyles.Left)));
            this.logPanel.BackColor = System.Drawing.SystemColors.GradientInactiveCaption;
            this.logPanel.Controls.Add(this.InfoBox);
            this.logPanel.Location = new System.Drawing.Point(7, 375);
            this.logPanel.Margin = new System.Windows.Forms.Padding(3, 2, 3, 2);
            this.logPanel.Name = "logPanel";
            this.logPanel.Padding = new System.Windows.Forms.Padding(3, 2, 3, 2);
            this.logPanel.Size = new System.Drawing.Size(281, 264);
            this.logPanel.TabIndex = 7;
            this.logPanel.TabStop = false;
            // 
            // Title
            // 
            this.Title.BackColor = System.Drawing.SystemColors.GradientInactiveCaption;
            this.Title.Font = new System.Drawing.Font("Microsoft Sans Serif", 10.8F, System.Drawing.FontStyle.Bold, System.Drawing.GraphicsUnit.Point, ((byte)(163)));
            this.Title.ForeColor = System.Drawing.Color.DarkRed;
            this.Title.Location = new System.Drawing.Point(218, 9);
            this.Title.Margin = new System.Windows.Forms.Padding(3, 2, 3, 2);
            this.Title.Name = "Title";
            this.Title.ReadOnly = true;
            this.Title.Size = new System.Drawing.Size(556, 28);
            this.Title.TabIndex = 8;
            this.Title.Text = "DECAL SCROLLING CONTROL";
            this.Title.TextAlign = System.Windows.Forms.HorizontalAlignment.Center;
            // 
            // RealtimeBox
            // 
            this.RealtimeBox.Anchor = ((System.Windows.Forms.AnchorStyles)((System.Windows.Forms.AnchorStyles.Top | System.Windows.Forms.AnchorStyles.Right)));
            this.RealtimeBox.Font = new System.Drawing.Font("Microsoft Sans Serif", 12F, System.Drawing.FontStyle.Regular, System.Drawing.GraphicsUnit.Point, ((byte)(163)));
            this.RealtimeBox.Location = new System.Drawing.Point(832, 11);
            this.RealtimeBox.Margin = new System.Windows.Forms.Padding(3, 2, 3, 2);
            this.RealtimeBox.Name = "RealtimeBox";
            this.RealtimeBox.ReadOnly = true;
            this.RealtimeBox.Size = new System.Drawing.Size(99, 30);
            this.RealtimeBox.TabIndex = 4;
            this.RealtimeBox.Text = "00:00:00";
            // 
            // controlPanel
            // 
            this.controlPanel.Anchor = ((System.Windows.Forms.AnchorStyles)(((System.Windows.Forms.AnchorStyles.Top | System.Windows.Forms.AnchorStyles.Left)
            | System.Windows.Forms.AnchorStyles.Right)));
            this.controlPanel.BackColor = System.Drawing.SystemColors.GradientInactiveCaption;
            this.controlPanel.Controls.Add(this.addDecal);
            this.controlPanel.Controls.Add(this.StopMotor);
            this.controlPanel.Controls.Add(this.RunMotor);
            this.controlPanel.Location = new System.Drawing.Point(7, 147);
            this.controlPanel.Margin = new System.Windows.Forms.Padding(3, 2, 3, 2);
            this.controlPanel.Name = "controlPanel";
            this.controlPanel.Padding = new System.Windows.Forms.Padding(3, 2, 3, 2);
            this.controlPanel.Size = new System.Drawing.Size(281, 224);
            this.controlPanel.TabIndex = 7;
            this.controlPanel.TabStop = false;
            this.controlPanel.Text = "CONTROL";
            // 
            // addDecal
            // 
            this.addDecal.BackColor = System.Drawing.Color.MediumSlateBlue;
            this.addDecal.Font = new System.Drawing.Font("Microsoft Sans Serif", 7.8F, System.Drawing.FontStyle.Bold, System.Drawing.GraphicsUnit.Point, ((byte)(163)));
            this.addDecal.ForeColor = System.Drawing.Color.White;
            this.addDecal.Location = new System.Drawing.Point(13, 17);
            this.addDecal.Margin = new System.Windows.Forms.Padding(3, 2, 3, 2);
            this.addDecal.Name = "addDecal";
            this.addDecal.Size = new System.Drawing.Size(251, 47);
            this.addDecal.TabIndex = 3;
            this.addDecal.Text = "ADD";
            this.addDecal.UseVisualStyleBackColor = false;
            this.addDecal.Click += new System.EventHandler(this.addDecal_Click);
            // 
            // StopMotor
            // 
            this.StopMotor.BackColor = System.Drawing.Color.Gold;
            this.StopMotor.Font = new System.Drawing.Font("Microsoft Sans Serif", 7.8F, System.Drawing.FontStyle.Bold, System.Drawing.GraphicsUnit.Point, ((byte)(163)));
            this.StopMotor.Location = new System.Drawing.Point(13, 146);
            this.StopMotor.Margin = new System.Windows.Forms.Padding(3, 2, 3, 2);
            this.StopMotor.Name = "StopMotor";
            this.StopMotor.Size = new System.Drawing.Size(251, 74);
            this.StopMotor.TabIndex = 2;
            this.StopMotor.Text = "STOP";
            this.StopMotor.UseVisualStyleBackColor = false;
            this.StopMotor.Click += new System.EventHandler(this.StopMotor_Click);
            // 
            // RunMotor
            // 
            this.RunMotor.BackColor = System.Drawing.Color.RoyalBlue;
            this.RunMotor.Font = new System.Drawing.Font("Microsoft Sans Serif", 7.8F, System.Drawing.FontStyle.Bold, System.Drawing.GraphicsUnit.Point, ((byte)(163)));
            this.RunMotor.ForeColor = System.Drawing.Color.White;
            this.RunMotor.Location = new System.Drawing.Point(13, 68);
            this.RunMotor.Margin = new System.Windows.Forms.Padding(3, 2, 3, 2);
            this.RunMotor.Name = "RunMotor";
            this.RunMotor.Size = new System.Drawing.Size(251, 74);
            this.RunMotor.TabIndex = 1;
            this.RunMotor.Text = "RUN";
            this.RunMotor.UseVisualStyleBackColor = false;
            this.RunMotor.Click += new System.EventHandler(this.RunMotor_Click);
            // 
            // cameraPanel
            // 
            this.cameraPanel.Anchor = ((System.Windows.Forms.AnchorStyles)((((System.Windows.Forms.AnchorStyles.Top | System.Windows.Forms.AnchorStyles.Bottom)
            | System.Windows.Forms.AnchorStyles.Left)
            | System.Windows.Forms.AnchorStyles.Right)));
            this.cameraPanel.BackColor = System.Drawing.SystemColors.GradientInactiveCaption;
            this.cameraPanel.Controls.Add(this.picCamera);
            this.cameraPanel.Location = new System.Drawing.Point(294, 43);
            this.cameraPanel.Margin = new System.Windows.Forms.Padding(3, 2, 3, 2);
            this.cameraPanel.Name = "cameraPanel";
            this.cameraPanel.Padding = new System.Windows.Forms.Padding(3, 2, 3, 2);
            this.cameraPanel.Size = new System.Drawing.Size(722, 596);
            this.cameraPanel.TabIndex = 7;
            this.cameraPanel.TabStop = false;
            this.cameraPanel.Text = "CAMERA";
            // 
            // picCamera
            // 
            this.picCamera.BackColor = System.Drawing.Color.Black;
            this.picCamera.Dock = System.Windows.Forms.DockStyle.Fill;
            this.picCamera.Location = new System.Drawing.Point(3, 17);
            this.picCamera.Name = "picCamera";
            this.picCamera.Size = new System.Drawing.Size(716, 577);
            this.picCamera.SizeMode = System.Windows.Forms.PictureBoxSizeMode.Zoom;
            this.picCamera.TabIndex = 0;
            this.picCamera.TabStop = false;
            // 
            // EXIT
            // 
            this.EXIT.Anchor = ((System.Windows.Forms.AnchorStyles)((System.Windows.Forms.AnchorStyles.Top | System.Windows.Forms.AnchorStyles.Right)));
            this.EXIT.BackColor = System.Drawing.Color.White;
            this.EXIT.Font = new System.Drawing.Font("Microsoft Sans Serif", 7.8F, System.Drawing.FontStyle.Bold, System.Drawing.GraphicsUnit.Point, ((byte)(163)));
            this.EXIT.ForeColor = System.Drawing.Color.Red;
            this.EXIT.Location = new System.Drawing.Point(956, 6);
            this.EXIT.Margin = new System.Windows.Forms.Padding(3, 2, 3, 2);
            this.EXIT.Name = "EXIT";
            this.EXIT.Size = new System.Drawing.Size(57, 31);
            this.EXIT.TabIndex = 4;
            this.EXIT.Text = "Quit";
            this.EXIT.UseVisualStyleBackColor = false;
            this.EXIT.Click += new System.EventHandler(this.EXIT_Click);
            // 
            // Form1
            // 
            this.AutoScaleMode = System.Windows.Forms.AutoScaleMode.None;
            this.ClientSize = new System.Drawing.Size(1028, 646);
            this.Controls.Add(this.logPanel);
            this.Controls.Add(this.EXIT);
            this.Controls.Add(this.cameraPanel);
            this.Controls.Add(this.controlPanel);
            this.Controls.Add(this.RealtimeBox);
            this.Controls.Add(this.Title);
            this.Controls.Add(this.uartPanel);
            this.FormBorderStyle = System.Windows.Forms.FormBorderStyle.None;
            this.Margin = new System.Windows.Forms.Padding(3, 2, 3, 2);
            this.MaximizeBox = false;
            this.MinimizeBox = false;
            this.Name = "Form1";
            this.StartPosition = System.Windows.Forms.FormStartPosition.CenterScreen;
            this.WindowState = System.Windows.Forms.FormWindowState.Maximized;
            this.Load += new System.EventHandler(this.Form1_Load);
            this.uartPanel.ResumeLayout(false);
            this.uartPanel.PerformLayout();
            this.logPanel.ResumeLayout(false);
            this.logPanel.PerformLayout();
            this.controlPanel.ResumeLayout(false);
            this.cameraPanel.ResumeLayout(false);
            ((System.ComponentModel.ISupportInitialize)(this.picCamera)).EndInit();
            this.ResumeLayout(false);
            this.PerformLayout();

        }

        #endregion
        private System.IO.Ports.SerialPort serialPort;
        private System.Windows.Forms.Timer timer1;
        private System.Windows.Forms.Button STATECONNECT;
        private System.Windows.Forms.TextBox UARTTITLEBOX;
        private System.Windows.Forms.GroupBox uartPanel;
        private System.Windows.Forms.TextBox InfoBox;
        private System.Windows.Forms.GroupBox logPanel;
        private System.Windows.Forms.TextBox Title;
        private System.Windows.Forms.TextBox RealtimeBox;
        private System.Windows.Forms.GroupBox controlPanel;
        private System.Windows.Forms.Button StopMotor;
        private System.Windows.Forms.Button RunMotor;
        private System.Windows.Forms.GroupBox cameraPanel;
        private System.Windows.Forms.PictureBox picCamera;
        private System.Windows.Forms.Button addDecal;
        private System.Windows.Forms.Button EXIT;
    }
}


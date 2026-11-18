\# MCU – ESP32 Firmware



\## 1. Overview | Tổng quan



\*\*Tiếng Việt\*\*



Thành phần MCU sử dụng vi điều khiển ESP32 để thực hiện các chức năng điều khiển ngoại vi và giao tiếp với Jetson Nano.



Chương trình được phát triển và nạp trực tiếp thông qua Arduino IDE.



\*\*English\*\*



The MCU module uses an ESP32 microcontroller to handle peripheral control and communication with the NVIDIA Jetson Nano.



The firmware is developed and uploaded using Arduino IDE.



\## 2. Hardware Requirements | Phần cứng



\- ESP32 Development Board – 36-pin.

\- Board selection: DOIT ESP32 DEVKIT V1.

\- USB cable for programming.

\- External peripherals according to the project configuration.



\## 3. Development Environment | Môi trường phát triển



\- Arduino IDE.

\- ESP32 Board Package.

\- USB-to-Serial driver (if required).



\## 4. GPIO Configuration | Cấu hình chân GPIO



The GPIO pin assignments are predefined in the source code.



\*\*Tiếng Việt\*\*



Các chân GPIO đã được khai báo sẵn trong chương trình. Người dùng cần đảm bảo kết nối phần cứng đúng với các chân được định nghĩa trong code.



Không tự ý thay đổi sơ đồ chân nếu chưa kiểm tra lại cấu hình phần cứng.



\## 5. Installation \& Upload | Cài đặt và nạp chương trình



\### Step 1 – Install Arduino IDE



Download and install Arduino IDE:



https://www.arduino.cc/en/software



\### Step 2 – Install ESP32 Board Package



1\. Open Arduino IDE.

2\. Open Boards Manager.

3\. Search for ESP32.

4\. Install the ESP32 board package.



\### Step 3 – Select Development Board



Navigate to:



\*\*Tools → Board → ESP32 Arduino → DOIT ESP32 DEVKIT V1\*\*



\### Step 4 – Select Communication Port



Connect the ESP32 to your computer using USB.



Navigate to:



\*\*Tools → Port → Select the ESP32 COM Port\*\*



\### Step 5 – Upload Firmware



1\. Open the MCU source code in Arduino IDE.

2\. Verify the GPIO configuration.

3\. Select the correct board and COM port.

4\. Click Verify to compile.

5\. Click Upload to flash the firmware.



\## 6. Notes | Lưu ý



\- Use the DOIT ESP32 DEVKIT V1 board profile.

\- Ensure the correct COM port is selected.

\- Verify GPIO connections before powering external peripherals.

\- UART communication settings must match the Jetson Nano configuration.



\- Chọn đúng board DOIT ESP32 DEVKIT V1.

\- Kiểm tra đúng cổng COM.

\- Kiểm tra kết nối GPIO trước khi cấp nguồn ngoại vi.

\- Cấu hình UART phải tương thích với Jetson Nano.


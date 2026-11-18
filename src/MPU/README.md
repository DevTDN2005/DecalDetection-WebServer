\# MPU – Jetson Nano Application



\## 1. Overview | Tổng quan



\*\*Tiếng Việt\*\*



Thành phần MPU là chương trình xử lý trung tâm được triển khai trên NVIDIA Jetson Nano 4GB.



Chương trình được phát triển bằng C++, thực hiện thu nhận hình ảnh từ camera, xử lý AI, theo dõi đối tượng, đếm số lượng decal, phát hiện lỗi và giao tiếp với các thành phần khác của hệ thống.



Dự án cung cấp script `build.sh` để tự động hóa quá trình biên dịch.



\*\*English\*\*



The MPU module is the main processing application deployed on the NVIDIA Jetson Nano 4GB.



Developed in C++, it handles camera acquisition, AI inference, object tracking, decal counting, defect detection, and communication with other system components.



A `build.sh` script is provided to automate the build process.



\## 2. Hardware Requirements | Phần cứng



\- NVIDIA Jetson Nano 4GB.

\- USB Camera.

\- ESP32 Development Board.

\- Compatible power supply.

\- Network connection (if required).



\## 3. Software Requirements | Phần mềm



\- NVIDIA JetPack.

\- CUDA.

\- TensorRT.

\- OpenCV.

\- C++ compiler.

\- CMake (if required by the build script).



\*Required software versions should match the project's tested environment.\*



\## 4. Installation \& Deployment | Cài đặt và triển khai



\### Step 1 – Prepare Source Code



\*\*Tiếng Việt\*\*



Tải mã nguồn thư mục `MPU` từ GitHub về Jetson Nano và mở Terminal tại thư mục chứa mã nguồn.



\*\*English\*\*



Download the `MPU` source code from GitHub to the Jetson Nano and open a terminal in the source directory.



\### Step 2 – Configure Build Script



Grant execution permission to the build script:



```bash

chmod +x build.sh

```



\### Step 3 – Build Application



Run the build script:



```bash

./build.sh

```



The script automatically performs the configured compilation steps and generates the application executable named `main`.



\## 5. AI Models | Mô hình AI



The application uses trained AI models for decal detection, counting, and defect inspection.



Model files and their configuration must be placed in the paths expected by the application.



\*\*Tiếng Việt\*\*



Các file model cần được đặt đúng đường dẫn mà chương trình quy định. Tham khảo `models/README.md` để biết cách chuẩn bị và cấu hình model.



\## 6. Notes | Lưu ý



\- The application is designed for NVIDIA Jetson Nano 4GB.

\- Use the provided `build.sh` script to build the application.

\- Ensure all required dependencies are installed.

\- Verify camera access and model paths before execution.

\- Ensure UART connections and communication parameters match the ESP32 firmware.

\- Chương trình được thiết kế cho NVIDIA Jetson Nano 4GB.

\- Sử dụng `build.sh` có sẵn để biên dịch.

\- Cài đặt đầy đủ các thư viện phụ thuộc.

\- Kiểm tra quyền truy cập camera và đường dẫn model trước khi chạy.

\- Kiểm tra kết nối UART và thông số giao tiếp với ESP32.


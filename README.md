# Industrial Decal Roll Inspection & Counting System
# Hệ thống kiểm tra lỗi và đếm số lượng nhãn decal trên cuộn

**An Embedded AI-Based Vision System for Real-Time Defect Detection and Production Counting**

**Hệ thống thị giác máy tính tích hợp AI trên nền tảng nhúng, phục vụ phát hiện lỗi và đếm số lượng nhãn decal trong dây chuyền sản xuất liên tục.**

[![Platform](https://img.shields.io/badge/Platform-NVIDIA%20Jetson%20Nano-green)](https://developer.nvidia.com/embedded/jetson-nano)
[![Language](https://img.shields.io/badge/Language-C%2B%2B-blue)](https://isocpp.org/)
[![Computer Vision](https://img.shields.io/badge/Computer%20Vision-YOLO-orange)](https://ultralytics.com/)
[![Deployment](https://img.shields.io/badge/Deployment-TensorRT-red)](https://developer.nvidia.com/tensorrt)

---

## 1. Project Overview | Tổng quan dự án

### Tiếng Việt

Dự án phát triển hệ thống kiểm tra chất lượng tự động bằng thị giác máy tính, nhằm phát hiện lỗi và đếm số lượng nhãn decal trên cuộn vật liệu đang di chuyển liên tục trong dây chuyền sản xuất.

Hệ thống sử dụng camera USB để thu nhận hình ảnh và NVIDIA Jetson Nano để xử lý trực tiếp bằng các mô hình Deep Learning.

**Các mục tiêu chính:**

- Phát hiện các nhãn decal bị lỗi trong quá trình sản xuất.
- Tự động đếm số lượng nhãn decal thông qua thuật toán phát hiện và theo dõi đối tượng.
- Xác định vị trí nhãn lỗi trong trình tự sản xuất.
- Lưu trữ hình ảnh lỗi phục vụ kiểm tra và truy xuất.
- Triển khai xử lý AI trực tiếp trên thiết bị nhúng Edge AI.

### English

This project develops an automated computer vision system for quality inspection and production counting of decal labels on a continuously moving material roll.

A USB camera captures the production images, while an NVIDIA Jetson Nano performs on-device processing using deep learning models.

**Main objectives:**

- Detect defective decal labels during production.
- Automatically count labels using object detection and tracking algorithms.
- Identify defective labels within the production sequence.
- Store defect images for quality inspection and traceability.
- Deploy AI inference directly on embedded Edge AI hardware.

---

## 2. System Architecture | Kiến trúc hệ thống

### Tiếng Việt

Hệ thống được chia thành các khối chức năng chính:

1. **Khối thu nhận hình ảnh:** Camera USB ghi nhận hình ảnh cuộn decal đang chuyển động.
2. **Khối xử lý AI:** Jetson Nano thực hiện nhận diện nhãn và phát hiện lỗi.
3. **Khối theo dõi và đếm:** ByteTrack kết hợp Line Crossing để xác định và đếm từng nhãn.
4. **Khối điều khiển:** ESP32 giao tiếp với Jetson Nano thông qua UART.
5. **Khối quản lý dữ liệu:** Lưu trữ kết quả kiểm tra và hình ảnh lỗi.

### English

The system consists of several functional modules:

1. **Image Acquisition:** A USB camera captures images of the moving decal roll.
2. **AI Processing:** Jetson Nano performs decal detection and defect inspection.
3. **Tracking and Counting:** ByteTrack and Line Crossing algorithms track and count individual decals.
4. **Control Module:** ESP32 communicates with Jetson Nano through UART.
5. **Data Management:** Inspection results and defect images are stored for further analysis.

---

## 3. Hardware Components | Phần cứng sử dụng

| Component | Description | Mô tả |
|---|---|---|
| NVIDIA Jetson Nano 4GB | Main AI processing unit | Bộ xử lý AI trung tâm |
| USB Camera | Image acquisition | Camera thu nhận hình ảnh |
| ESP32 | Peripheral control | Điều khiển ngoại vi |
| Conveyor System | Material transportation | Hệ thống vận chuyển cuộn decal |
| Power Supply | System power management | Cấp nguồn hệ thống |

---

## 4. Software & Technologies | Công nghệ sử dụng

| Technology | Application | Ứng dụng |
|---|---|---|
| C++ | Main application | Phát triển chương trình chính |
| Python | Model training | Huấn luyện mô hình |
| OpenCV | Image processing | Xử lý hình ảnh |
| YOLO | Object detection | Phát hiện đối tượng |
| ByteTrack | Object tracking | Theo dõi đối tượng |
| TensorRT | AI inference optimization | Tối ưu suy luận AI |
| ONNX | Model interoperability | Định dạng trao đổi mô hình |
| UART | Device communication | Giao tiếp thiết bị |
| CMake | Build system | Biên dịch chương trình |

---

## 5. Core Functional Modules | Các chức năng chính

### 5.1. Decal Detection & Counting | Nhận diện và đếm nhãn decal

**Tiếng Việt**

Chức năng đếm sử dụng mô hình YOLO để phát hiện nhãn decal, kết hợp ByteTrack để duy trì ID của từng đối tượng qua các khung hình liên tiếp.

Một đường đếm ảo được thiết lập tại vị trí trung tâm vùng quan sát. Khi đối tượng được theo dõi đi qua đường đếm, hệ thống ghi nhận một sự kiện đếm.

**Quy trình xử lý:**

1. Thu nhận hình ảnh từ camera USB.
2. Nhận diện nhãn decal bằng mô hình YOLO.
3. Theo dõi đối tượng bằng ByteTrack.
4. Xác định vị trí đối tượng so với đường đếm.
5. Phát hiện sự kiện Line Crossing.
6. Cập nhật số lượng sản phẩm.

**English**

The counting module uses a YOLO detection model combined with ByteTrack to maintain object identities across consecutive frames.

A virtual counting line is positioned at the center of the inspection area. Whenever a tracked decal crosses this line, the system registers a counting event.

**Processing pipeline:**

1. Capture frames from the USB camera.
2. Detect decal labels using YOLO.
3. Track detected objects using ByteTrack.
4. Monitor object positions relative to the counting line.
5. Detect line-crossing events.
6. Update the production counter.

### 5.2. Defect Detection | Phát hiện lỗi sản phẩm

**Tiếng Việt**

Mô hình AI riêng biệt được phát triển nhằm nhận diện các nhãn decal không đạt yêu cầu chất lượng.

Các mục tiêu bao gồm:

- Phát hiện vùng bất thường trên nhãn decal.
- Xác định vị trí lỗi.
- Liên kết nhãn lỗi với trình tự sản xuất.
- Lưu hình ảnh nhãn lỗi phục vụ truy xuất.

Dữ liệu huấn luyện hướng đến việc kết hợp cả mẫu đạt yêu cầu (OK) và mẫu lỗi (NG), giúp mô hình phân biệt tốt hơn giữa biến thiên bình thường và lỗi thực tế.

**English**

A separate AI model is developed to identify defective decal labels.

Its objectives include:

- Detecting abnormal regions on decal labels.
- Localizing detected defects.
- Associating defective labels with their production sequence.
- Saving defect images for traceability.

The training dataset incorporates both good (OK) and defective (NG) samples to improve the model's ability to distinguish normal variations from actual defects.

### 5.3. Embedded AI Deployment | Triển khai AI trên nền tảng nhúng

**Tiếng Việt**

Các mô hình sau khi huấn luyện được chuyển đổi và tối ưu hóa để triển khai trên NVIDIA Jetson Nano thông qua TensorRT.

**Quy trình triển khai:**

```text
Dataset Preparation
        |
        v
Model Training
        |
        v
Model Validation
        |
        v
ONNX Export
        |
        v
TensorRT Engine
        |
        v
C++ Inference on Jetson Nano
```

**English**

Trained models are converted and optimized for NVIDIA Jetson Nano deployment using TensorRT.

The deployment pipeline includes dataset preparation, model training, validation, ONNX export, TensorRT engine generation, and C++ inference integration.

---

## 6. Experimental Results | Kết quả thực nghiệm

### Tiếng Việt

Hệ thống đã được thử nghiệm trong điều kiện cuộn decal chuyển động liên tục.

| Tiêu chí | Kết quả hiện tại |
|---|---|
| Nền tảng xử lý | Jetson Nano 4GB |
| Camera | USB Camera |
| Thuật toán đếm | YOLO + ByteTrack + Line Crossing |
| Vị trí đường đếm | Trung tâm vùng quan sát |
| Tốc độ vận hành ổn định quan sát được | Khoảng 30 SPS |

**Nhận xét:**

- Chức năng đếm hoạt động ổn định trong điều kiện thử nghiệm thông thường.
- Ở tốc độ dây chuyền cao, hệ thống có thể bỏ sót đối tượng.
- Hiện tượng nhòe chuyển động ảnh hưởng đến khả năng phát hiện lỗi.
- Việc thu nhận đầy đủ hình ảnh của từng nhãn là yêu cầu quan trọng đối với chức năng kiểm tra lỗi.

*Lưu ý: 30 SPS là mức vận hành ổn định quan sát được trong quá trình thử nghiệm, chưa phải kết quả benchmark được xác nhận bằng quy trình đo kiểm tiêu chuẩn.*

### English

The system has been tested under continuous decal roll movement conditions.

| Parameter | Current Result |
|---|---|
| Processing Platform | Jetson Nano 4GB |
| Camera | USB Camera |
| Counting Algorithm | YOLO + ByteTrack + Line Crossing |
| Counting Line | Center of inspection area |
| Observed Stable Operating Speed | Approximately 30 SPS |

**Observations:**

- The counting module operates reliably under normal test conditions.
- Higher production speeds may cause missed detections.
- Motion blur affects defect detection performance.
- Capturing complete decal images is essential for accurate defect inspection.

*Note: Approximately 30 SPS represents the currently observed stable operating point, not a formally validated benchmark.*

---

## 7. Repository Structure | Cấu trúc thư mục

```text
DAKTNC-TH/
│
├── src/
│   ├── main.cpp
│   ├── camera/
│   ├── detection/
│   ├── tracking/
│   ├── counting/
│   └── communication/
│
├── include/
│
├── models/
│   └── README.md
│
├── training/
│   ├── dataset/
│   ├── scripts/
│   └── README.md
│
├── docs/
│   ├── architecture/
│   ├── hardware/
│   └── experiments/
│
├── images/
│
├── scripts/
│
├── CMakeLists.txt
├── .gitignore
└── README.md
```

*This is a suggested repository structure. Actual directories should reflect the implemented source code.*

*Cấu trúc trên là đề xuất tổ chức repository. Các thư mục thực tế cần được điều chỉnh theo source code đã triển khai.*

---

## 8. Installation & Deployment | Cài đặt và triển khai

### Requirements | Yêu cầu

- NVIDIA Jetson Nano 4GB
- Compatible JetPack version
- CUDA
- TensorRT
- OpenCV
- CMake
- C++ compiler

### Clone Repository | Tải mã nguồn

```bash
git clone <YOUR_REPOSITORY_URL>
cd DAKTNC-TH
```

### Build Project | Biên dịch

```bash
mkdir build
cd build

cmake ..
make -j$(nproc)
```

### Run Application | Chạy chương trình

```bash
./<EXECUTABLE_NAME>
```

*Model files and hardware-specific configurations must be prepared before execution.*

*Các file model và cấu hình phần cứng cần được chuẩn bị trước khi chạy chương trình.*

---

## 9. Limitations & Future Improvements | Hạn chế và hướng phát triển

### Current Limitations | Hạn chế hiện tại

- Detection performance may decrease at high conveyor speeds.
- Motion blur affects defect recognition accuracy.
- Small and subtle defects require further dataset improvements.
- Complete decal image acquisition requires further optimization.

- Hiệu suất nhận diện có thể giảm khi dây chuyền hoạt động ở tốc độ cao.
- Nhòe chuyển động ảnh hưởng đến độ chính xác phát hiện lỗi.
- Các lỗi nhỏ và khó nhận biết cần được cải thiện thêm thông qua dữ liệu huấn luyện.
- Cần tối ưu quá trình thu nhận hình ảnh để đảm bảo lưu được toàn bộ nhãn decal.

### Future Improvements | Hướng phát triển

- Improve defect detection accuracy through dataset refinement.
- Optimize TensorRT inference performance.
- Improve camera acquisition and synchronization.
- Enhance defect localization and production traceability.
- Develop a production monitoring interface.

- Nâng cao độ chính xác phát hiện lỗi thông qua cải thiện dữ liệu huấn luyện.
- Tối ưu hiệu suất suy luận bằng TensorRT.
- Cải thiện quá trình thu nhận và đồng bộ hình ảnh.
- Nâng cao khả năng định vị lỗi và truy xuất sản phẩm.
- Phát triển giao diện giám sát và thống kê sản xuất.

---

## 10. Documentation | Tài liệu kỹ thuật

### Tiếng Việt

Tài liệu chi tiết của dự án sẽ được tổ chức trong thư mục `docs/`, bao gồm:

- Sơ đồ khối phần cứng và phần mềm.
- Hướng dẫn lắp đặt và kết nối thiết bị.
- Kiến trúc chương trình và luồng xử lý.
- Quy trình huấn luyện và triển khai mô hình AI.
- Quy trình chuẩn bị và gán nhãn dữ liệu.
- Kết quả thử nghiệm và đánh giá hiệu năng.

### English

Detailed technical documentation will be maintained in the `docs/` directory, including:

- Hardware and software architecture diagrams.
- Hardware installation and connection guides.
- Software architecture and processing pipeline.
- AI model training and deployment procedures.
- Dataset preparation and annotation guidelines.
- Experimental results and performance evaluation.

---

## 11. Author | Tác giả

**Trần Đại Nghĩa - Neil**, **Nguyễn Ngọc Gia Hân**, **Huỳnh Chí Vỹ**, **Nguyễn Quang Thuận**

Embedded Systems | Computer Vision | Edge AI

Academic engineering research project focused on embedded AI deployment and automated industrial visual inspection.

Dự án nghiên cứu kỹ thuật tập trung vào triển khai AI trên nền tảng nhúng và tự động hóa kiểm tra chất lượng sản phẩm bằng thị giác máy tính.

---

**Disclaimer:** This repository is intended for educational, research, and engineering development purposes. Features and performance figures are documented according to their actual implementation and testing status.

**Lưu ý:** Repository phục vụ mục đích học tập, nghiên cứu và phát triển kỹ thuật. Các chức năng và thông số hiệu năng được trình bày dựa trên tình trạng triển khai và thử nghiệm thực tế.

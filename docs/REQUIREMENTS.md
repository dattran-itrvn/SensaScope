# SensaPulse — Yêu cầu firmware v1.0 & v1.1

## Giới thiệu

SensaPulse là một thiết bị đeo (wearable) cỡ nhỏ dùng để thu dữ liệu nghiên cứu — chủ yếu là âm thanh tim/phổi/ho và chuyển động cơ thể. Người dùng đeo liên tục 24/7. Toàn bộ dữ liệu được ghi thô lên thẻ micro-SD, không xử lý DSP trên thiết bị. Phân tích, tách kênh, gán nhãn... đều làm offline bằng Python.

Có hai phiên bản firmware:

- **v1.0** chỉ ghi cục bộ. Người dùng tự rút thẻ ra để lấy dữ liệu.
- **v1.1** thêm BLE để PC kết nối và tải các session về.


## Phần cứng

PCB v1.0 đã có sẵn. Sơ đồ chân nằm trong tài liệu hardware riêng. Tóm tắt các khối firmware cần làm việc với:

| Khối | Linh kiện | Giao tiếp |
|---|---|---|
| MCU | nRF52840 (module Ebyte E73-2G4M08S1C) | — |
| Mic body | ST MP23DB01HPTR — PDM, kênh trái | PDM (dùng chung) |
| Mic ambient | ST IMP34DT05TR — PDM, kênh phải | PDM (dùng chung) |
| IMU | ST LSM6DSL, I²C addr 0x6A | I²C |
| Storage | micro-SD ở SPI mode | SPI |
| Nguồn | Li-ion → SW1 (switch nguồn cứng) → LDO 3.3V | — |
| LED | active-high qua N-MOSFET | 1 GPIO |
| Đo pin | Cầu chia 1:2 → ADC | 1 GPIO |


## Firmware v1.0 — ghi cục bộ

### Ghi dữ liệu

Audio thu stereo 16 kHz, 16-bit, 2 kênh (kênh trái = mic body, kênh phải = mic ambient), ghi ra file WAV chuẩn với header 44 byte.

IMU lấy mẫu 52 Hz, lưu raw LSB của cả accel và gyro vào CSV. Header `t_us,ax,ay,az,gx,gy,gz`, mỗi dòng một mẫu, `t_us` là thời điểm tính bằng microsecond kể từ lúc thiết bị bật.

Mỗi lần ghi tương đương một session, tạo thành một thư mục riêng trên thẻ tên dạng `SESSION_NNNNN` (5 chữ số, tăng dần, lưu lại trong `/SD/sync_state.json` để giữ thứ tự sau khi mất điện hoặc reboot). Trong thư mục có:

- `audio.wav`
- `imu.csv`
- `meta.json` chứa `session_id`, `start_uptime_ms`, `fs_audio`, `fs_imu`, `fw_version`, `fw_build_hash` (git short SHA, embed lúc build), `batt_mv_start`, `chip_id` và `device_name`.
- `.unsynced` — file rỗng 0 byte, chỉ được tạo *sau khi* đã có dữ liệu ghi xuống đĩa thật sự. Thư mục thiếu file này coi như session lỗi (crash trước khi ghi được gì), tool đồng bộ ở v1.1 sẽ bỏ qua.

Cứ mỗi 10 phút thiết bị tự cắt sang folder mới mà không gián đoạn dòng ghi — khoảng gap audio chấp nhận tối đa 100 ms.

### Trigger và state machine

Người dùng double-tap vào thiết bị để bắt đầu hoặc dừng ghi. Tap được phát hiện bằng hardware tap detector tích hợp của LSM6DSL, không poll mềm.

Thiết bị có bốn trạng thái:

| State | Khi nào | LED | Chuyển sang |
|---|---|---|---|
| `IDLE` | Sau khi boot, pin còn đủ. | Một nháy 100 ms mỗi 3 giây. | Tap → `RECORDING`. Pin < 3300 mV → `LOW_BATT`. |
| `RECORDING` | Vừa bắt đầu một session. | Hai nháy 100 ms cách nhau 200 ms, mỗi 5 giây. | Tap → `IDLE`. Pin tụt → `LOW_BATT`. Ghi lỗi không phục hồi được → `ERROR`. |
| `LOW_BATT` | Pin dưới ngưỡng. | Nháy nhanh 5 Hz. | Pin ≥ 3500 mV → `IDLE`. |
| `ERROR` | SD đầy hoặc ghi lỗi liên tục. | SOS Morse. | Reboot. |

LED dùng 24/7 nên tổng tiêu thụ trung bình phải dưới khoảng 1 mA — đó là lý do dùng pattern thưa thay vì sáng liên tục.

### Pin

ADC đọc qua cầu chia 1:2, convert ra mV thật. Poll 30 giây một lần khi đang `RECORDING` hoặc `LOW_BATT`. Ngưỡng tắt ghi là 3300 mV, ngưỡng cho phép ghi lại là 3500 mV — chừa 200 mV hysteresis để tránh nháy state khi pin lưng chừng.

### Định danh thiết bị

Lúc boot, firmware đọc file `/SD/device.name` (UTF-8, tối đa 32 byte, trim whitespace). Nếu file vắng hoặc rỗng thì fallback thành `chip_<hex>` với phần hex lấy từ chip ID FICR. Tên này dùng trong `meta.json` ở v1.0 và sẽ dùng cho BLE advertise ở v1.1.

### Yêu cầu chất lượng

Trong điều kiện bình thường (nhiệt độ ≤ 30 °C, thẻ SanDisk class 10 trở lên), một session phải đạt:

- Audio không có byte 0 chèn vào giữa do thread stall.
- WAV header lúc đóng file có `data_bytes` đúng bằng số byte data thật.
- Tần số IMU thực tế trong khoảng 51.5 – 52.5 Hz.
- Drift giữa audio và IMU dưới 100 ms trong một session 10 phút.
- Không có sample nào bị drop.

Riêng phần ghi thẻ phải chịu được lỗi tạm thời. Thẻ SD đôi khi trả về lỗi I/O thoáng qua khi đang busy bên trong (garbage collection, wear-levelling). Firmware không được vì một lần lỗi mà mất luôn cả session — phải có cơ chế thử lại và phục hồi.

### Tiêu chí nghiệm thu

Để được đóng v1.0, firmware phải qua hết các bài kiểm tra sau:

1. Một session 25 phút (đi qua ít nhất 2 lần đổi folder) chạy sạch, không drop sample, drift audio↔IMU dưới 100 ms.
2. Bấm tap để start → stop → start lại liên tục 5 lần, mỗi lần đều sinh ra đủ folder với 4 file và có marker `.unsynced`.
3. Hạ điện áp xuống dưới 3300 mV trên bench, thiết bị dừng và vào `LOW_BATT`; nâng lên trên 3500 mV thì quay về `IDLE`.
4. Reboot lạnh 10 lần liên tiếp, lần nào cũng init được thẻ SD.
5. Build hash trong `meta.json` khớp với `git rev-parse --short HEAD`.
6. Bài stress nặng: chạy 30 phút với rotation rút xuống 1 phút/folder (gấp 10 lần production). Không được vào `ERROR`, tất cả folder phải hợp lệ (đủ file và có `.unsynced`).

### Ngoài phạm vi v1.0

Mọi thứ liên quan đến BLE, USB CDC, xử lý tín hiệu trên thiết bị (filter, feature extraction), đồng hồ thực, OTA, pairing và mã hoá đều không nằm trong v1.0.


## Firmware v1.1 — BLE sync với PC

v1.1 cộng thêm một service BLE để PC có thể quét, kết nối, và tải các session về máy. Giao thức chi tiết (UUID, opcode, định dạng frame, cơ chế ACK) tách thành tài liệu riêng — phần dưới chỉ mô tả mức yêu cầu.

### BLE service

Service custom có 4 characteristic:

- **Device Info** (read): trả về một JSON nhỏ `{name, chip_id, fw_version, fw_build_hash, fs_audio, fs_imu, batt_mv}` để PC nhận diện.
- **Control** (write + notify): nơi PC gửi các opcode `LIST`, `READ`, `ACK`, `ABORT`, `DEL`, `RESET`.
- **Data** (notify only): kênh stream nội dung file khi đang `READ`.
- **Set Name** (write): cập nhật `/SD/device.name`.

Thiết bị chỉ advertise khi đang ở trạng thái `IDLE`. Khi vào `RECORDING` thì tắt BLE controller — vừa để tiết kiệm pin, vừa để giải phóng radio.

### Quản lý dung lượng

Trước khi tạo session mới, nếu dung lượng trống dưới 100 MB, firmware tự xoá folder synced cũ nhất (folder đã nhận ACK từ PC, không còn `.unsynced`). Xoá đến khi đủ chỗ. Trường hợp tất cả folder vẫn còn `.unsynced` mà thẻ đầy thì từ chối ghi tiếp và vào `ERROR` — không được phép đè dữ liệu chưa được đồng bộ.

### Quy tắc state

Thêm trạng thái `SYNC`. Các transition mới và các transition bị cấm:

- `IDLE → SYNC` khi BLE connect.
- `SYNC → IDLE` khi BLE disconnect.
- `RECORDING → SYNC` bị cấm — PC connect trong lúc đang ghi sẽ bị reject.
- `SYNC → RECORDING` bị cấm — double-tap trong lúc sync bị bỏ qua.
- `ERROR → SYNC` được phép, để user vẫn còn cách vớt dữ liệu khi thẻ đầy.

### PC tool

Tool Python (`tools/sync.py`, dựa trên `bleak`) làm các bước: scan → connect → LIST → READ (có resume từ offset nếu mất kết nối giữa chừng) → ACK → thiết bị xoá `.unsynced`. PC ghi file vào thư mục tạm trước, chỉ rename sang thư mục cuối khi đã ACK xong — tránh trạng thái nửa vời nếu hủy ngang.

### Throughput

Mục tiêu: một session 10 phút (~38 MB) sync xong trong 10 phút. Yêu cầu BLE 5.0 PHY 2M, MTU 247, connection interval 7.5 ms. Vì hầu hết điện thoại từ chối interval dưới 15 ms, v1.1 chỉ hỗ trợ PC. Mobile để dành cho phiên bản sau.

### Bảo mật

v1.1 không xác thực, không mã hoá — chấp nhận được cho giai đoạn dev. Pairing, AES-GCM và các bước hardening khác phải hoàn thành trước khi thiết bị rời môi trường bench. Checklist hardening tách thành tài liệu riêng.

### Tiêu chí nghiệm thu

1. PC quét → connect → LIST → READ → ACK trên 5 session liên tiếp không drop byte nào.
2. Trong lúc đang sync, double-tap phải bị bỏ qua (không vào `RECORDING`).
3. Trong lúc đang record, PC connect phải bị reject.
4. Sync 38 MB trong dưới 10 phút trên Mac M-series với USB BLE adapter.
5. Tự ngắt kết nối giữa chừng một lệnh `READ`, kết nối lại, dữ liệu resume đúng offset, không trùng cũng không sót.
6. Bench eviction: ghi đầy thẻ, kiểm tra folder synced cũ nhất bị xoá đúng và folder chưa sync vẫn nguyên.

### Ngoài phạm vi v1.1

Pairing và mã hoá payload, OTA firmware update, app di động, streaming audio realtime qua BLE, và auto-sync khi cắm dock đều không nằm trong v1.1.

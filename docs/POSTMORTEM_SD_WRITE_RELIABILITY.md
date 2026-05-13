# Postmortem — SD write reliability crash (task #32)

Ngày: 2026-05-12. Phạm vi: firmware SensaPulse v1.0 trên PCB v1.0 (nRF52840 + SD SPI 24 MHz + 2 mic PDM + IMU LSM6DSL). Tài liệu này tổng hợp toàn bộ quá trình điều tra một lỗi đã làm tốn ~2 ngày: production firmware rơi vào trạng thái `ERROR` (LED SOS) sau 12-41 phút ghi, mà các fix ban đầu trên SD driver, retry, card hardware đều **không** giải quyết được.

Mục tiêu của tài liệu:
- Để session Claude Code sau (và dev team đọc tham khảo) có thể lần lại được cách chẩn đoán, không lặp lại sai lầm.
- Ghi nhận lại nguyên tắc kiến trúc bị vi phạm dẫn đến bug.

---

## 1. Triệu chứng

Trên bench, sau khi double-tap để start session:

- **Audio + IMU ghi tốt 10-40 phút đầu** (1-4 folder hoàn chỉnh ~38.4 MB audio + 1.1 MB IMU).
- Sau đó **FSM nhảy thẳng sang ERROR**, LED chuyển từ "RECORDING" sang "SOS Morse".
- Folder cuối thường có nội dung nhưng nhỏ hơn 38.4 MB (writer chết giữa session).

RTT log lúc fail luôn có chuỗi:
```
fs: file write error (-5)
sd_writer: fs_write(6400 B) try 1/3: -5  ─┐
sd_writer: fs_write(6400 B) try 2/3: -5  │  3 lần retry × 200 ms backoff
sd_writer: fs_write(6400 B) try 3/3: -5  ─┘
sd_writer: audio fs_write at N B: -5
sd: Failed to read from SDMMC -116        ← SDMMC driver hard timeout (-ETIME)
sd: Card read failed
session: monitor: writer trouble — aborting SESSION_NNNNN
main: FSM: session aborted by watchdog → ERROR
```

Tức là: tầng `sd_writer` retry 3 lần (theo task #30), không qua được; tầng SDHC SPI driver báo `-116` (card không phản hồi trong deadline); watchdog của `session.c monitor` thấy `sd_writer_failed()` → abort.

---

## 2. Các giả thuyết ban đầu (đều SAI)

Các hướng đã chạy theo và bị fail:

### 2.1 "Card SD bị EOL"

- **Lý do nghĩ**: pattern `-116 ETIME` đúng signature của hardware-class wear (task #29 đã ghi cho card 30 GB SanDisk hồi #28).
- **Bằng chứng**: lần đầu fail trên 122 GB card "bench-trusted" → nghi card này cũng đang xuống cấp.
- **Bị bác bỏ**: format lại card, fail lại sau ~5 phút (sớm hơn) chứ không lâu hơn. Một thẻ mới mua cũng fail tương tự (sẽ phân tích bên dưới).

### 2.2 "Cadence rotation 1 phút quá nặng"

- **Lý do nghĩ**: test stress với rotation 1 phút (gấp 10× production) crash sớm; production rotation 10 phút.
- **Bằng chứng tưởng có**: cadence càng nặng càng dễ fail.
- **Bị bác bỏ**: production cadence 10 phút vẫn fail 12-41 phút sau. Rate ghi giống nhau giữa hai chế độ.

### 2.3 "Silent FAT corruption"

- **Lý do nghĩ**: trong một lần test có folder `SESSION_00007` xuất hiện là 0-byte file (không phải folder!) mặc dù `mkdir/open/write` đều trả 0.
- **Fix đã thử (#30 Layer A + B + C)**:
  - `fs_stat` post-mkdir để verify `FS_DIR_ENTRY_DIR`.
  - Retry helper cho `fs_open/write/close/seek` (3× × 200 ms backoff).
  - Defer-on-fail cho rotation (giữ folder cũ, retry sau).
- **Kết quả**: stress 1 phút × 30 phút pass clean (32 folder OK). **Nhưng production 10 phút vẫn fail sau 12-41 phút**.
- **Bài học**: #30 đúng (bug silent corruption thật và có defense), nhưng KHÔNG fix bug chính. Đừng dừng ở fix đầu tiên thấy có vẻ hợp lý.

### 2.4 "Boot timing margin"

- **Lý do nghĩ**: IMU đôi khi báo `WHO_AM_I=0xFF` ở boot. Nghi LDO + LSM6DSL chưa ổn.
- **Bị parked**: dự kiến thành task #31 (delay 100 ms đầu `main()`), không phải root cause.

---

## 3. Bisect bằng các test cô lập

Sau khi các fix "bằng trực giác" thất bại, chuyển sang **chiến lược cô lập từng layer**. Tách sd_stress firmware thành các test progressively-richer:

| # | Test name | Cấu hình | Mục đích | Kết quả |
|---|---|---|---|---|
| 1 | `sd_only` | Mount SD, ghi 6.4 KB/100 ms vào 1 file, fs_sync 5s | SD driver + FATFS + card + bus có sạch không? | ✅ 484 MB / 1h, 0 lỗi |
| 2 | `sd + PDM discard` | Test 1 + PDM peripheral chạy thật, discard slab ngay (không qua FATFS) | DMA arbitration giữa PDM ↔ SPI có phá nhau không? | ✅ 487 MB / 1h, 0 lỗi |
| 5 | `full data path` | audio.c + imu.c + imu_sampler.c + sd_writer.c, không rotation, không FSM | Pipeline full producer + consumer có lỗi? | ✅ 213 MB / 55 phút, 0 lỗi |
| 6 | Test 5 + rotation | Thêm rotation 10 phút (gọi `sd_writer_rotate_full` trực tiếp từ main thread, không FSM) | Rotation logic có gây lỗi? | ✅ 5 rotations / 1h, 0 lỗi |
| 7 | Test 5 + monitor mock | Thêm 1 k_timer + k_work mỗi giây gọi `fs_open + fs_close` trên `/SD:/marker.bin` từ `system_work_queue` | Replicate session.c monitor's touch_unsynced từ ngoài sd_writer thread | ❌ **CRASH sau 86 giây** với đúng signature `-116` của production |

(Test 3 và Test 4 không cần chạy — Test 7 đã pin được root cause.)

Tổng hợp dòng test:

```
SD subsystem     ✅       ─────────┐
+ PDM DMA        ✅                │
+ full producers ✅                │  ⇒ Lớp dữ liệu hoàn toàn sạch
+ rotation       ✅       ─────────┘
+ monitor mock   ❌  ←── Đây là điểm break
```

---

## 4. Root cause

**Vi phạm "single-FATFS-owner" invariant của task #25.**

Khi tái cấu trúc data path (#25), một quyết định kiến trúc đã được chốt:

> Chỉ duy nhất `sd_writer` thread được phép gọi FATFS API. Mọi thread khác (producer audio, producer IMU, monitor watchdog, FSM main loop) chỉ được push data vào FIFO hoặc dùng API public của sd_writer. Lý do: SD card + sdhc_spi driver không tolerate concurrent FATFS access tốt; khi 2 thread tranh chấp volume mutex, card vào trạng thái busy lâu và rốt cuộc timeout.

`session.c` đã vi phạm chỗ này ở 2 nơi:

### 4.1 `monitor_work_handler.touch_unsynced` (chính)

Sau mỗi 1 giây trong khi RECORDING, monitor work-item chạy trên `system_work_queue` gọi:

```c
fs_open(&f, "/SD:/SESSION_NNNNN/.unsynced", FS_O_CREATE | FS_O_WRITE);
fs_close(&f);
```

Tuy hành động "đúng" (chỉ 1 lần thật-sự fire khi `unsynced_marker_id != current_id`), nhưng:

1. Mỗi tick monitor đều CHECK điều kiện → mỗi tick có **chance** gọi fs_open nếu marker chưa set.
2. Bản thân `fs_stat` (qua nội bộ FATFS) để check existence cũng cần FAT-table read.
3. Quan trọng nhất: lần đầu marker được tạo (đầu mỗi session sau rotate), fs_open thực sự fire cùng lúc với sd_writer đang fs_write audio block → 2 thread đụng FATFS mutex → card vào burst busy → timeout.

### 4.2 `rotate_work_handler.statvfs_free_mb` (phụ)

Trước mỗi rotation, rotate work-item gọi `fs_statvfs(SD_MOUNT_POINT, ...)` để check free space và log warning. Cũng từ `system_work_queue`. Mặc dù chỉ chạy mỗi 10 phút, vẫn là extra disk I/O burst ngay TRƯỚC khi `sd_writer_rotate_full` đẩy luôn 10+ FATFS ops của rotate sequence → card overload.

### 4.3 `save_counter` trong `session_start` (phụ)

`save_counter` gọi fs_open + fs_write + fs_close trên `/SD/sync_state.json`. Hàm này được gọi từ main thread NGAY SAU `sd_writer_start()` đã spawn writer thread. Tức là 2 thread cùng đụng FATFS — tuy chỉ trong vài chục mili-giây đầu session, vẫn là vi phạm.

---

## 5. Bằng chứng định lượng

Test 7 reproduce chính xác failure signature trong 86 giây với cùng pattern thread access:

```
t=76s : audio=2.93 MB, marker_touches=45 (1/s), marker_err=0     [OK]
t=81s : audio FROZEN at 2.93 MB, imu_drop=136, marker_err=1     [stuck]
t=86s : drop=370, marker_err=7
        sd: Failed to read from SDMMC -116                       [hard timeout]
        sd: Card read failed
        sd_writer: audio fs_write -EIO
        test7: WRITER FAILED
```

Phân biệt với:
- **Test 5** (cùng data path, KHÔNG có monitor mock) → 0 lỗi qua 55 phút.
- **Test 6** (cùng + rotation) → 0 lỗi qua 1 giờ.

⇒ Yếu tố duy nhất khác biệt: **fs_open+close 1Hz từ `system_work_queue` trong khi sd_writer đang fs_write**.

Test 7 stress nặng hơn production (1/s vs 1/session), giải thích tại sao Test 7 chết sớm (86s) còn production chết muộn (12-41 phút) — bản chất bug là tích lũy stress chứ không phải race điểm cố định.

---

## 6. Fix (#32)

Đưa **mọi** FATFS op của `session.c` về sd_writer thread:

### 6.1 `sd_writer.c/h` — API mới

```c
int sd_writer_touch_file(const char *path);
```

Đồng bộ, dùng cùng pattern `k_sem` như `sd_writer_rotate_full`:
- Caller submit path qua mutex-protected buffer + set atomic `touch_req`.
- Caller `k_sem_take(&touch_done, K_SECONDS(5))`.
- Writer thread polls `touch_req` trong main loop, giữa drain → fs_open + fs_close (với retry helpers của #30) → set result → `k_sem_give`.

### 6.2 `session.c monitor_work_handler.touch_unsynced`

```c
// Trước:
fs_open(&f, path, FS_O_CREATE | FS_O_WRITE);
fs_close(&f);

// Sau:
sd_writer_touch_file(path);
```

### 6.3 `session.c rotate_work_handler`

Xoá hoàn toàn cuộc gọi `statvfs_free_mb()`. Khi task #17 (eviction) làm tới, route qua API mới `sd_writer_get_free_mb()` (chưa làm — pending #17).

### 6.4 `session.c session_start`

Đảo thứ tự: `save_counter()` chạy **trước** `sd_writer_start()`. Tại thời điểm `save_counter()` chạm FATFS, sd_writer thread chưa tồn tại → không contend.

---

## 7. Bài học

### 7.1 Kiến trúc

- **"Single-FATFS-owner" là invariant nghiêm ngặt, không phải guideline.** Vi phạm dù 1 lần/session vẫn đủ để stress card → timeout. Mọi FATFS call từ ngoài sd_writer thread cần route qua API public sync. Dev sau này nếu thêm fs_op ở `session.c` hay `main.c`, phải kiểm tra context thread.

- **SD SPI driver + FATFS volume mutex không phải mọi-thread-an-toàn theo nghĩa "tolerate" mà chỉ "không panic"**. Khi 2 thread tranh, card đầu kia (firmware nội bộ) phải sequence các transaction → bị stress → timeout `-116`. Mutex chỉ ngăn race ở phần mềm, không bảo vệ card vật lý.

### 7.2 Debug

- **Đừng bị anchor bởi triệu chứng "hardware-class"**. `-116 ETIME` trông như card chết, nhưng thật ra là consequence của software contention. Format lại card, đổi card mới đều không cứu được.

- **Bisect bằng cô lập layer mạnh hơn fix trực giác**. Sau 4 fix sai (#23, #25, #27, #30) cho cùng triệu chứng, kế hoạch cô lập (Test 1 → Test 7) cho ra root cause chính xác chỉ trong 5 lần test.

- **Reproduce phải chạy đủ thời gian**. Production crash 12-41 phút; nếu chỉ test 5 phút có thể không gặp. Mỗi test trong bisect phải ≥ window đó (chọn 1 giờ để có margin).

- **Có RTT log khi crash là điều kiện cần để chẩn đoán đúng.** Lần đầu user mô tả "SOS rồi" nhưng không có RTT log → đoán mò sai 2 hướng. Sau khi có log thì pin được chính xác `-116` ở tầng SDMMC driver.

### 7.3 Kiểm tra lúc PR review

Khi review code đụng FATFS, hỏi 3 câu:

1. **Hàm này chạy trên thread nào?** (System work-queue? Main? sd_writer? Producer?)
2. **Có lúc nào sd_writer thread đang chạy đồng thời không?** (Tra theo lifecycle: trước/sau `sd_writer_start`/`stop`)
3. **Nếu có → đã route qua API của sd_writer chưa?**

Nếu trả lời "không/không/không" → reject.

---

## 8. Reference

- Task #25 `sd_writer` single-thread architecture: `TASKS.md` mục #25.
- Task #30 retry + verify + defer-on-fail: `TASKS.md` mục #30.
- Task #32 (fix này): `TASKS.md` mục #32.
- `CLAUDE.md` Discovered entry liên quan: dòng 2026-05-12.
- Test 7 firmware (reproduction): `git log app/sd_stress/src/main.c` — commit branch `fix/30-sd-write-resilience`.

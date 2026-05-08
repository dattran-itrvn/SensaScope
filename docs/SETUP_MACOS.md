# SensaPulse FW — Toolchain Setup (macOS Apple Silicon)

Tested target: macOS 14+ on M-series (M1/M2/M3/M4). All components below have native ARM64 builds — không cần Rosetta.

Toolchain target: nRF Connect SDK (Zephyr-based) for nRF52840 / E73-2G4M08S1C module, flashed via Segger J-Link over SWD.

---

## 0. Pre-flight check

Mở Terminal:

```bash
# Architecture (phải là arm64)
uname -m

# macOS version (>= 14 Sonoma)
sw_vers
```

Cài Homebrew nếu chưa có:

```bash
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
```

Sau khi cài Homebrew, chạy 2 dòng nó in ra cuối quá trình cài (thường là `eval "$(/opt/homebrew/bin/brew shellenv)"` thêm vào `~/.zshrc`).

---

## 1. Cài Segger J-Link Software

J-Link là driver + GUI để probe nói chuyện với chip qua SWD. Nordic dùng `nrfjprog` để gọi xuống J-Link DLL.

1. Vào https://www.segger.com/downloads/jlink/
2. Tải **J-Link Software and Documentation Pack for macOS (Apple Silicon)** — file `.pkg`.
3. Cài bình thường (Next, Next, Done).
4. Cắm J-Link vào USB → kiểm tra:

   ```bash
   JLinkExe -device nRF52840_xxAA -if SWD -speed 4000
   ```

   Nếu thấy `J-Link>` prompt là OK. Gõ `q` để thoát. (Chưa cần kết nối tới board.)

---

## 2. Cài nRF Command Line Tools

Bộ này có `nrfjprog` (CLI flash/erase/reset) — Zephyr `west flash` gọi xuống nó.

1. Vào https://www.nordicsemi.com/Products/Development-tools/nrf-command-line-tools/download
2. Chọn **macOS (arm64)** → tải `.dmg` → kéo `nRF Command Line Tools.app` vào `/Applications`.
3. Mở app **một lần** để macOS chấp nhận unsigned binary, hoặc:

   ```bash
   xattr -dr com.apple.quarantine "/Applications/nRF Command Line Tools.app"
   ```

4. Verify:

   ```bash
   nrfjprog --version
   # Expect: nrfjprog version: 10.x.y
   ```

---

## 3. Cài nRF Connect for Desktop

Đây là launcher chứa nhiều app con (Toolchain Manager, Programmer, Bluetooth Low Energy app, etc.).

1. Vào https://www.nordicsemi.com/Products/Development-tools/nrf-connect-for-desktop/download
2. Tải **macOS (Apple Silicon, .dmg)** → cài vào `/Applications`.
3. Mở `nRF Connect for Desktop`.
4. Trong launcher, **Install** 2 app:
   - **Toolchain Manager** (cài SDK + Zephyr toolchain)
   - **Programmer** (GUI để flash/erase nhanh khi không muốn dùng CLI)

---

## 4. Cài nRF Connect SDK qua Toolchain Manager

1. Mở **Toolchain Manager** từ nRF Connect for Desktop.
2. Chọn install location mặc định `~/ncs` (gọn nhất).
3. Click **Install** ở phiên bản **stable mới nhất** (chọn LTS hoặc latest, tránh `main`/`master` để code khỏi đổi liên tục).
4. Chờ ~10–20 phút (download SDK + toolchain GNU Arm Embedded + west + Python venv).
5. Sau khi xong, click mũi tên kế bên SDK → **Open Terminal** → đây là shell có sẵn `west`, `cmake`, `arm-none-eabi-gcc`, `python` venv của Zephyr. Verify:

   ```bash
   west --version
   arm-none-eabi-gcc --version
   python --version
   ```

   Tip: Toolchain Manager có nút "Open VS Code" — click để mở VS Code đã có path đúng.

---

## 5. Cài VS Code + nRF Connect Extension Pack

1. Tải VS Code: https://code.visualstudio.com/download (Mac Apple Silicon).
2. Mở VS Code → tab Extensions → search **"nRF Connect for VS Code Extension Pack"** (publisher: nordic-semiconductor) → Install. Bộ này tự kéo theo:
   - nRF Connect for VS Code (build/flash/debug GUI)
   - nRF DeviceTree
   - nRF Kconfig
   - nRF Terminal
   - C/C++ extension
   - CMake Tools

3. Khởi động lại VS Code → ở thanh sidebar trái sẽ có icon **nRF Connect** (con gà gáy của Nordic).

4. Mở extension → **Manage toolchains** → nó tự nhận SDK đã cài ở bước 4.

---

## 6. Smoke test với example có sẵn

Trước khi build code SensaPulse, mình build thử `blinky` để chắc toolchain ngon:

```bash
# Mở Terminal từ Toolchain Manager (để có west trong PATH)
cd ~/ncs/v2.X.X/zephyr/samples/basic/blinky
west build -b nrf52840dk/nrf52840 -p auto
# Build xong sẽ thấy build/zephyr/zephyr.hex

# Cắm J-Link vào board (chưa cần đúng board, chỉ test toolchain)
west flash --runner jlink   # hoặc bỏ qua nếu chưa có hardware
```

Nếu `west build` xong không lỗi là setup thành công. (Lệnh `west flash` sẽ fail nếu chưa cắm board, mình bỏ qua bước đó cho bring-up sau.)

---

## 7. Xác minh cuối — checklist

- [ ] `uname -m` → `arm64`
- [ ] `nrfjprog --version` → in version
- [ ] `JLinkExe -device nRF52840_xxAA -if SWD -speed 4000` → vào J-Link prompt
- [ ] Toolchain Manager → SDK cài thành công, ít nhất 1 version đang ✓
- [ ] `west --version` (chạy trong Toolchain Manager terminal) → in version
- [ ] VS Code có sidebar "nRF Connect"
- [ ] Build thử `blinky` cho `nrf52840dk/nrf52840` thành công

Khi cả 7 mục đều ✓, báo lại em sẽ tạo board definition cho `sensapulse_v1` và blink LED đầu tiên trên hardware thật.

---

## Troubleshooting nhanh

| Triệu chứng | Cách xử lý |
|---|---|
| `nrfjprog: command not found` | App đã cài nhưng PATH chưa có. Thêm `export PATH="/Applications/nRF Command Line Tools.app/Contents/Resources/bin:$PATH"` vào `~/.zshrc`. |
| `JLinkExe: Permission denied` cắm USB | System Settings → Privacy & Security → cho phép Segger driver. Rút cắm lại J-Link. |
| `west build` báo `CMake Error: Could not find toolchain` | Mở Terminal qua Toolchain Manager, không qua Terminal thường — vì Toolchain Manager set sẵn `ZEPHYR_BASE`, `PATH`, `ZEPHYR_TOOLCHAIN_VARIANT`. |
| Apple gatekeeper chặn binary | `xattr -dr com.apple.quarantine /Applications/<App>.app` |
| nRF Connect cài extension pack báo conflict | Disable hết extension C/C++ cũ, để Nordic extension tự cài bản nó cần. |

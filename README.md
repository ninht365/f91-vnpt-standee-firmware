# F91 VNPT Standee Firmware v2.3 (ESP32-S2 + ST7789 8-bit Parallel)

Firmware điều khiển hiển thị và giao tiếp cho thiết bị **F91 VNPT Standee / Loa Thanh Toán Thông Minh** trên nền tảng vi điều khiển **ESP32-S2** và màn hình **ST7789** chuẩn giao tiếp song song 8-bit Intel 8080 ($240 \times 320$ pixel).

---

## 🌟 Tính Năng Nổi Bật

- **Màn hình chờ thương hiệu VNPT HD:** Nền trắng tinh khiết chuẩn 240x320 pixel với logo VNPT sắc nét, không viền rác.
- **Bộ sinh mã QR động Nayuki ISO/IEC 18004:** Tự động tạo mã QR chất lượng cao (ECC High/Medium) theo nội dung thanh toán gửi từ máy tính/POS, tích hợp logo VNPT ($36 \times 36$) ở tâm mã QR.
- **Huy hiệu đếm ngược thời gian (Countdown Badge):** Đếm lùi thời gian thực (`30s`, `29s`, ..., `1s`), tự động đóng mã QR và quay về màn hình chờ khi hết hạn. Được đồng bộ bằng FreeRTOS Mutex, không bị kẹt hay giật lag.
- **Hỗ trợ nạp luồng ảnh thô 150KB Zero-Copy (`AT+DTIME`):** Nhận trực tiếp ảnh $240 \times 320$ RGB565 qua USB CDC và vẽ trực tiếp vào LCD, không tốn RAM hệ thống.
- **Giao diện dòng lệnh AT Command chuẩn hoá:** Tự động Echo ký tự khi gõ/paste (hỗ trợ phím Backspace, Terminal VS Code, SSCOM, PuTTY, v.v.).

---

## 📡 Bảng Tập Lệnh Serial AT Commands Chi Tiết

Mọi lệnh AT được gửi qua cổng **USB Serial CDC (Baudrate: 115200, kết thúc bằng `\r\n` hoặc `\n`)**.

| STT | Câu lệnh AT | Cú pháp & Ví dụ | Mô tả chi tiết | Phản hồi từ thiết bị |
| :---: | :--- | :--- | :--- | :--- |
| **1** | **`AT`** | `AT` | Kiểm tra kết nối Serial giữa máy tính/POS và thiết bị. | `OK` |
| **2** | **`AT+VER`** | `AT+VER` | Lấy thông tin phiên bản firmware đang chạy. | `+VER: F91_VNPT_STANDEE_V2.0`<br>`OK` |
| **3** | **`ATE1`** | `ATE1` | **Bật chế độ Serial Echo** (Mặc định). Ký tự khi gõ hoặc paste vào terminal sẽ hiển thị thời gian thực. Hỗ trợ phím `Backspace`. | `OK` |
| **4** | **`ATE0`** | `ATE0` | **Tắt chế độ Serial Echo**. Dành cho các phần mềm POS/máy tự động giao tiếp không muốn nhận lại chuỗi gửi. | `OK` |
| **5** | **`AT+QR`** | `AT+QR=https://vnpt.vn,30`<br>`AT+QR="https://youtube.com",45` | **Sinh và hiển thị mã QR thanh toán**. Có thể bọc chuỗi URL trong dấu ngoặc kép hoặc không.<br>- Tham số 1: Nội dung mã QR.<br>- Tham số 2: Thời gian đếm ngược (giây). | `+QR: OK`<br>*(hoặc `+QR: ERROR` nếu chuỗi rỗng)* |
| **6** | **`AT+QR_DISPLAY`** | `AT+QR_DISPLAY=30,https://vnpt.vn` | **Lệnh hiển thị mã QR theo chuẩn gốc F91 Hemipay** (tương thích ngược hoàn toàn).<br>- Tham số 1: Thời gian đếm ngược (giây).<br>- Tham số 2: Nội dung mã QR. | `+QR_DISPLAY: OK` |
| **7** | **`AT+BG`**<br>**`AT+CLEAR`** | `AT+BG`<br>`AT+CLEAR`<br>`AT+DISPLAY_CLEAR` | **Quay về màn hình chờ VNPT ngay lập tức**. Xóa sạch mọi mã QR và hủy bỏ hoàn toàn tác vụ đếm ngược. | `OK` |
| **8** | **`AT+DTIME=0`** | `AT+DTIME=0` | **Chuyển sang chế độ nhận luồng ảnh thô 153.600 bytes** ($240 \times 320$ RGB565) hiển thị vĩnh viễn trên LCD. | `OK`<br>*(Sau khi nhận đủ 150KB: `+DTIME: OK`)* |
| **9** | **`AT+DTIME=<sec>`** | `AT+DTIME=30` | **Nhận luồng ảnh thô 153.600 bytes** kèm thời gian đếm lùi `<sec>` giây trước khi tự động về màn hình chờ. | `OK`<br>*(Sau khi nhận đủ 150KB: `+DTIME: OK`)* |

---

## 🛠️ Sơ Đồ Chân Phần Cứng (Hardware Pinout)

Thiết bị sử dụng chip **ESP32-S2** điều khiển màn hình **ST7789 240x320** qua bus vi xử lý 8-bit Intel 8080:

### 1. Đường Dữ Liệu 8-bit (Data Bus D0 - D7)
| ST7789 Data Pin | ESP32-S2 GPIO |
| :--- | :--- |
| **D0** | `GPIO 33` |
| **D1** | `GPIO 34` |
| **D2** | `GPIO 35` |
| **D3** | `GPIO 36` |
| **D4** | `GPIO 37` |
| **D5** | `GPIO 38` |
| **D6** | `GPIO 9` |
| **D7** | `GPIO 10` |

### 2. Đường Điều Khiển Bus (Control Pins)
| Chân tín hiệu | ESP32-S2 GPIO | Trạng thái tích cực / Chức năng |
| :--- | :--- | :--- |
| **CS (Chip Select)** | `GPIO 11` | Mức thấp (`0`) khi truyền lệnh/dữ liệu |
| **DC / RS (Data/Command)** | `GPIO 13` | Mức thấp (`0`): Command / Mức cao (`1`): Data |
| **WR (Write Enable)** | `GPIO 12` | Kích sườn xung lên (`0` ➔ `1`) để chốt byte |
| **RST (Reset)** | `GPIO 21` | Reset cứng phần cứng ST7789 |
| **BK_LIGHT (Backlight)** | `GPIO 18` | Mức cao (`1`): Bật đèn nền LED màn hình |

### 3. Nguồn & Kích Hoạt Phần Cứng
| Chức năng | ESP32-S2 GPIO | Mức logic thiết lập |
| :--- | :--- | :--- |
| **Power Control 1** | `GPIO 42` | `1` (Cấp nguồn VDD LCD) |
| **Power Control 2** | `GPIO 14` | `1` (Enable) |
| **Auxiliary Pins** | `GPIO 40`, `GPIO 41` | `0` (Ground reference) |

---

## 🚀 Hướng Dẫn Biên Dịch & Nạp Firmware

### 1. Biên dịch (Build)
Mở cửa sổ PowerShell có môi trường ESP-IDF (v5.x / v6.x) và chạy:
```powershell
cd C:\Users\ADMIN\.gemini\antigravity\scratch\f91_lcd_test
idf.py build
```

### 2. Nạp Firmware vào thiết bị (Flash)
1. Đưa thiết bị vào chế độ ROM Bootloader:
   - **Nhấn giữ nút BOOT** trên mạch.
   - **Nhấn nhả nút RESET**.
   - **Thả nút BOOT**.
2. Chạy script nạp tự động tìm cổng COM:
   ```powershell
   .\flash_com.ps1
   ```
3. Sau khi nạp thành công 100%, **nhấn nút RESET** trên mạch để khởi động thiết bị.

---

## 🖥️ Hướng Dẫn Mở Terminal Để Gửi Lệnh

### Cách 1: Sử dụng Terminal VS Code (`idf.py monitor`)
Chạy trực tiếp trong terminal VS Code:
```powershell
idf.py monitor
```
*(Bạn có thể gõ hoặc paste trực tiếp bất kỳ lệnh AT nào. Nhấn `Ctrl + ]` để thoát).*

### Cách 2: Sử dụng Extension Serial Monitor của VS Code
1. Cài đặt tiện ích **Serial Monitor** trong VS Code.
2. Chọn đúng cổng COM của F91 Standee, chọn Baudrate **115200**.
3. Bấm **Start Monitoring** và gửi các câu lệnh AT ở ô gửi lệnh.

---

## 🖼️ Hướng Dẫn Tải Ảnh Thô (Raw Image Upload)

Chế độ ảnh thô cho phép hiển thị hình ảnh đồ họa tùy ý (poster quảng cáo, banner khuyến mãi, hình ảnh thanh toán tùy biến) trực tiếp lên màn hình $240 \times 320$.

### 🌟 Cách 1: Nạp ảnh tự động bằng Script Python (Nhanh nhất & Tự động Resize)
Dự án đã tích hợp sẵn công cụ [`send_image.py`](file:///C:/Users/ADMIN/.gemini/antigravity/scratch/f91_lcd_test/send_image.py). Công cụ này sẽ tự động đọc file ảnh (JPG/PNG/BMP), resize về $240 \times 320$, chuyển đổi hệ màu 16-bit RGB565 và bắn qua cổng COM trong 1 giây.

- **Hiển thị vĩnh viễn (cho đến khi có lệnh khác):**
  ```powershell
  python send_image.py my_poster.jpg
  ```
- **Hiển thị có đếm lùi thời gian (ví dụ 30 giây):**
  ```powershell
  python send_image.py my_poster.png -t 30
  ```
- **Chỉ định cổng COM cụ thể (nếu cắm nhiều thiết bị):**
  ```powershell
  python send_image.py my_poster.jpg -p COM5
  ```

---

### 🛠️ Cách 2: Tạo file `.bin` bằng phần mềm Image2Lcd và gửi qua Serial Tool (SSCOM / TeraTerm)
1. Mở phần mềm **Image2Lcd**:
   - Chọn ảnh bất kỳ cần nạp.
   - **Output file type:** `*.bin`
   - **BitsPixel:** `16bit TrueColor` (RGB565).
   - **Max Width & Height:** `240` x `320`.
   - **Scan mode:** `Horizon scan` / `Top to bottom`.
   - Bỏ chọn ô *Include Head Data* (để file xuất ra có dung lượng đúng $153.600$ bytes).
   - Bấm **Save** để lưu file `image.bin`.
2. Mở phần mềm **SSCOM / TeraTerm / Serial Monitor**:
   - Bước 1: Gửi lệnh text `AT+DTIME=0` (hoặc `AT+DTIME=30`). Thiết bị sẽ trả về `OK`.
   - Bước 2: Dùng chức năng **Send File / Send Binary Data** để truyền file `image.bin` vừa tạo.
   - Thiết bị sẽ vẽ từng pixel trực tiếp lên màn hình và trả về `+DTIME: OK`.


import sys
import os
import time
import struct
import argparse

try:
    from PIL import Image
except ImportError:
    print("Vui long cai dat Pillow: pip install Pillow")
    sys.exit(1)

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    print("Vui long cai dat pyserial: pip install pyserial")
    sys.exit(1)

LCD_WIDTH = 240
LCD_HEIGHT = 320
TOTAL_BYTES = LCD_WIDTH * LCD_HEIGHT * 2  # 153,600 bytes

def find_esp_port():
    ports = list(serial.tools.list_ports.comports())
    for p in ports:
        desc = (p.description or "").lower()
        hwid = (p.hwid or "").lower()
        if "303a" in hwid or "esp32" in desc or "usb serial" in desc or "ch340" in desc or "cp210" in desc:
            return p.device
    if len(ports) > 0:
        return ports[0].device
    return None

def convert_image_to_rgb565(image_path):
    img = Image.open(image_path).convert('RGB')
    img = img.resize((LCD_WIDTH, LCD_HEIGHT), Image.Resampling.LANCZOS)
    
    raw_data = bytearray()
    pixels = list(img.getdata())
    
    for r, g, b in pixels:
        r5 = (r >> 3) & 0x1F
        g6 = (g >> 2) & 0x3F
        b5 = (b >> 3) & 0x1F
        rgb565 = (r5 << 11) | (g6 << 5) | b5
        raw_data.extend(struct.pack('>H', rgb565))
        
    return raw_data

def main():
    parser = argparse.ArgumentParser(description="Upload anh tho RGB565 len F91 VNPT Standee qua lenh AT+DTIME")
    parser.add_argument("image", help="Duong dan den file anh (JPG, PNG, BMP, ...)")
    parser.add_argument("-p", "--port", help="Cong COM (Vi du: COM5). Tu dong tim neu de trong.")
    parser.add_argument("-t", "--timeout", type=int, default=0, help="Thoi gian hien thi (giay). 0 = hien thi vinh vien (mac dinh: 0)")
    parser.add_argument("-b", "--baud", type=int, default=115200, help="Baudrate (mac dinh: 115200)")
    parser.add_argument("--save-bin", help="Luu ra file .bin tho (153.600 bytes)")
    
    args = parser.parse_args()

    if not os.path.isfile(args.image):
        print(f"[ERROR] File anh khong ton tai: {args.image}")
        sys.exit(1)

    if args.image.lower().endswith(".bin") or os.path.getsize(args.image) == TOTAL_BYTES:
        print(f"[1/3] Dang doc truc tiep file binary: {args.image}...")
        with open(args.image, "rb") as f:
            raw_bytes = f.read()
        print(f"      Dung luong byte anh tho: {len(raw_bytes):,} bytes")
    else:
        print(f"[1/3] Dang doc va chuyen doi anh: {args.image} -> 240x320 RGB565...")
        raw_bytes = convert_image_to_rgb565(args.image)
        print(f"      Dung luong byte anh tho: {len(raw_bytes):,} bytes (Chuan 153.600 bytes)")

    if len(raw_bytes) != TOTAL_BYTES:
        print(f"[WARNING] Kich thuoc du lieu la {len(raw_bytes)} bytes (Yeu cau chuan: 153.600 bytes)!")

    if args.save_bin:
        with open(args.save_bin, "wb") as f:
            f.write(raw_bytes)
        print(f"      Da luu file binary: {args.save_bin}")

    port = args.port or find_esp_port()
    if not port:
        print("[ERROR] Khong tim thay cong COM cua ESP32. Vui long cam thiet bi hoac chi dinh bang -p COMx")
        sys.exit(1)

    print(f"[2/3] Ket noi den cong {port} (Baudrate: {args.baud})...")
    try:
        ser = serial.Serial(port, args.baud, timeout=3)
    except Exception as e:
        print(f"[ERROR] Khong the mo cong {port}: {e}")
        sys.exit(1)

    time.sleep(0.5)
    ser.reset_input_buffer()
    ser.reset_output_buffer()

    cmd = f"AT+DTIME={args.timeout}\r\n"
    print(f"[3/3] Gui lenh: {cmd.strip()}")
    ser.write(cmd.encode('utf-8'))

    response = ser.readline().decode('utf-8', errors='ignore')
    print(f"      Phan hoi thiet bi: {response.strip()}")

    print("      Dang ban luong anh tho (153.600 bytes) truc tiep vao LCD...")
    start_time = time.time()
    
    chunk_size = 512
    total_len = len(raw_bytes)
    sent_len = 0

    for i in range(0, total_len, chunk_size):
        chunk = raw_bytes[i:i+chunk_size]
        ser.write(chunk)
        sent_len += len(chunk)
        percent = (sent_len / total_len) * 100
        print(f"\r      Tien do: {sent_len:,}/{total_len:,} bytes ({percent:.1f}%)", end='', flush=True)
        time.sleep(0.010)

    ser.flush()
    print()
    elapsed = time.time() - start_time
    print(f"      Da gui xong trong {elapsed:.2f}s (Toc do: {len(raw_bytes)/elapsed/1024:.1f} KB/s)")

    # Doi phan hoi ket qua
    ser.timeout = 5
    result = ser.readline().decode('utf-8', errors='ignore')
    if result.strip():
        print(f"      Ket qua: {result.strip()}")

    ser.close()
    print("[THANH CONG] Anh da duoc ve len toan bo man hinh F91 Standee!")

if __name__ == "__main__":
    main()

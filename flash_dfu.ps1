$MERGED_BIN = Join-Path $PSScriptRoot "build\merged_flash.bin"
Write-Host "Kiểm tra thiết bị DFU (ESP32-S2)..." -ForegroundColor Cyan

$dfuList = dfu-util --list 2>&1
if ($dfuList -notmatch "303a:0002") {
    Write-Host "❌ Không tìm thấy ESP32-S2 ở DFU mode!" -ForegroundColor Red
    Write-Host "👉 Hãy giữ nút BOOT rồi bấm nút RESET để vào DFU mode." -ForegroundColor Yellow
    exit 1
}

Write-Host "Đang nạp firmware: $MERGED_BIN ..." -ForegroundColor Green
dfu-util --device 303a:0002 --alt 0 --dfuse-address 0x00000000 --download "$MERGED_BIN" --reset

if ($LASTEXITCODE -eq 0) {
    Write-Host "`n✅ Nạp firmware thành công! Màn hình F91 sẽ tự động khởi động và chạy smoke test." -ForegroundColor Green
} else {
    Write-Host "`n❌ Có lỗi xảy ra trong quá trình nạp qua dfu-util." -ForegroundColor Red
}

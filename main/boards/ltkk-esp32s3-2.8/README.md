# LTKK ESP32-S3 2.8-inch Display

Firmware Xiaozhi mang bộ nhận diện **LTKK - Lập Trình Không Khó** cho bo
2.8inch ESP32-S3 Display của LCDWIKI (ES3C28P có cảm ứng / ES3N28P không cảm ứng).

Board này dùng chung sơ đồ chân với `freenove-esp32s3-display-2.8-lcd`.

## Phần cứng
- ESP32-S3, 16MB flash, 8MB PSRAM (OPI)
- Màn hình ILI9341V 2.8" 320x240 (SPI), cảm ứng FT6336G (I2C 0x38)
- Codec ES8311 + ampli FM8002E, micro MEMS
- LED RGB IO42, nút BOOT IO0, đo pin IO9

## Giao diện Lu - LTKK
- Màn hình chào: logo LTKK + "Học để làm", 2,5 giây (`ltkk_brand.h`)
- Khuôn mặt Lu: đôi mắt robot xanh ngọc, 10 biểu cảm, tự chớp mắt (`ltkk_display.cc`)
- Màn hình chờ: đồng hồ lớn, thứ/ngày, gợi ý "Nói Hi ESP hoặc chạm để gọi Lu"
- Đang nghe: sóng âm cam/xanh + "Lu đang nghe nè…"
- Hẹn giờ học: vòng đếm ngược trên màn hình chờ, báo chuông khi hết giờ
- Giao diện tối (mặc định): nền navy có mạch điện; giao diện sáng: nền trắng xanh
- Ảnh giao diện sinh tự động: `ltkk_ui_images.c` (đôi mắt, chữ số, chữ tiếng Việt)

## Ra lệnh bằng giọng nói (MCP)
- "Hẹn giờ học 25 phút" / "Nhắc mình sau 10 phút"
- "Thêm 5 phút nữa"
- "Còn bao lâu nữa?"
- "Huỷ hẹn giờ"
- "Chuyển sang giao diện sáng / tối", "Tăng độ sáng màn hình", "Giảm âm lượng"

## Thao tác
- Chạm 1 lần: bắt đầu / dừng trò chuyện
- Chạm 2 lần: bắt đầu nghe
- Giữ hơn 3 giây: vào chế độ cài Wi-Fi

## Build
```bash
python scripts/build.py ltkk-esp32s3-2.8 --language vi-VN
```
Hoặc dùng GitHub Actions: workflow "LTKK - Build firmware".
Nạp file `build/merged-binary.bin` vào địa chỉ `0x0`.

#pragma once

// =====================================================================
//  BỘ NHẬN DIỆN LTKK - LẬP TRÌNH KHÔNG KHÓ
//  Muốn đổi màu: sửa các mã 0xRRGGBB bên dưới rồi build lại.
// =====================================================================

// Màu gốc lấy từ logo
#define LTKK_COLOR_NAVY          0x0B1F4D   // xanh navy (chữ "KK")
#define LTKK_COLOR_BLUE          0x0A5BD8   // xanh dương (chữ "LT")
#define LTKK_COLOR_CYAN          0x12C2F0   // xanh ngọc (viền sáng)
#define LTKK_COLOR_ORANGE        0xFF7A1A   // cam nhấn (mũi tên)
#define LTKK_COLOR_WHITE         0xFFFFFF

// Màn hình chào khi khởi động
#define LTKK_SPLASH_ENABLE       1          // 1 = bật, 0 = tắt
#define LTKK_SPLASH_MS           2500       // thời gian hiển thị (mili giây)

// 1 = dùng hình nền mạch điện LTKK cho giao diện tối, 0 = nền màu trơn
#define LTKK_DARK_BACKGROUND_IMAGE 1

class Display;

namespace ltkk {

// Lần đầu khởi động: chọn giao diện tối (navy) làm mặc định.
// Gọi TRƯỚC khi tạo màn hình.
void SetDefaultThemeDark();

// Tô lại 2 giao diện "light" và "dark" của Xiaozhi theo màu LTKK.
// Gọi NGAY SAU khi tạo màn hình (display_ = new SpiLcdDisplay(...)).
void ApplyTheme();

// Hiện màn hình chào: logo LTKK + "Học để làm".
void ShowSplash(Display* display);

}  // namespace ltkk

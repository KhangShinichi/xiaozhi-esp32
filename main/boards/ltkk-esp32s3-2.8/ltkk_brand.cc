#include "ltkk_brand.h"

#include <esp_log.h>
#include <lvgl.h>

#include <memory>

#include "display/display.h"
#include "display/lvgl_display/lvgl_image.h"
#include "display/lvgl_display/lvgl_theme.h"
#include "settings.h"

#define TAG "LtkkBrand"

extern "C" {
extern const lv_image_dsc_t ltkk_splash_image;  // ltkk_splash_image.c
extern const lv_image_dsc_t ltkk_background;    // ltkk_background.c
}

namespace ltkk {

namespace {

// Bọc ảnh nằm sẵn trong flash để gán làm hình nền theme
class StaticImage : public LvglImage {
public:
    explicit StaticImage(const lv_image_dsc_t* dsc) : dsc_(dsc) {}
    const lv_img_dsc_t* image_dsc() const override { return dsc_; }

private:
    const lv_image_dsc_t* dsc_;
};

}  // namespace

void SetDefaultThemeDark() {
    Settings settings("display", true);
    if (settings.GetString("theme", "").empty()) {
        settings.SetString("theme", "dark");
    }
}

void ApplyTheme() {
    auto& themes = LvglThemeManager::GetInstance();

    // Giao diện tối: nền navy + mạch điện, bong bóng chat xanh / cam
    if (auto dark = themes.GetTheme("dark")) {
        dark->set_background_color(lv_color_hex(LTKK_COLOR_NAVY));
        dark->set_text_color(lv_color_hex(LTKK_COLOR_WHITE));
        dark->set_chat_background_color(lv_color_hex(LTKK_COLOR_NAVY));
        dark->set_user_bubble_color(lv_color_hex(LTKK_COLOR_ORANGE));
        dark->set_assistant_bubble_color(lv_color_hex(LTKK_COLOR_BLUE));
        dark->set_system_bubble_color(lv_color_hex(0x13306B));
        dark->set_system_text_color(lv_color_hex(0xBFD9FF));
        dark->set_border_color(lv_color_hex(LTKK_COLOR_CYAN));
        dark->set_low_battery_color(lv_color_hex(0xE53935));
#if LTKK_DARK_BACKGROUND_IMAGE
        dark->set_background_image(std::make_shared<StaticImage>(&ltkk_background));
#endif
    }

    // Giao diện sáng: nền trắng xanh nhạt, chữ navy
    if (auto light = themes.GetTheme("light")) {
        light->set_background_color(lv_color_hex(0xF2F7FF));
        light->set_text_color(lv_color_hex(LTKK_COLOR_NAVY));
        light->set_chat_background_color(lv_color_hex(0xF2F7FF));
        light->set_user_bubble_color(lv_color_hex(0xFFD9BF));
        light->set_assistant_bubble_color(lv_color_hex(0xD6E6FF));
        light->set_system_bubble_color(lv_color_hex(LTKK_COLOR_WHITE));
        light->set_system_text_color(lv_color_hex(LTKK_COLOR_BLUE));
        light->set_border_color(lv_color_hex(LTKK_COLOR_BLUE));
        light->set_low_battery_color(lv_color_hex(0xE53935));
    }
    ESP_LOGI(TAG, "LTKK theme applied");
}

void ShowSplash(Display* display) {
#if LTKK_SPLASH_ENABLE
    if (display == nullptr) {
        return;
    }
    DisplayLockGuard lock(display);

    // Vẽ trên lớp trên cùng nên không đụng vào giao diện gốc của Xiaozhi
    lv_obj_t* splash = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(splash);
    lv_obj_set_size(splash, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(splash, lv_color_hex(LTKK_COLOR_NAVY), 0);
    lv_obj_set_style_bg_grad_color(splash, lv_color_hex(LTKK_COLOR_BLUE), 0);
    lv_obj_set_style_bg_grad_dir(splash, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_main_stop(splash, 90, 0);
    lv_obj_set_style_bg_opa(splash, LV_OPA_COVER, 0);
    lv_obj_add_flag(splash, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* img = lv_image_create(splash);
    lv_image_set_src(img, &ltkk_splash_image);
    lv_obj_center(img);

    // Thanh màu xanh -> cam ở mép dưới, giống viền logo
    lv_obj_t* bar = lv_obj_create(splash);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LV_PCT(100), 4);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(LTKK_COLOR_BLUE), 0);
    lv_obj_set_style_bg_grad_color(bar, lv_color_hex(LTKK_COLOR_ORANGE), 0);
    lv_obj_set_style_bg_grad_dir(bar, LV_GRAD_DIR_HOR, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);

    // Mờ dần rồi tự xóa
    lv_obj_fade_out(splash, 400, LTKK_SPLASH_MS);
    lv_obj_delete_delayed(splash, LTKK_SPLASH_MS + 450);
#endif
}

}  // namespace ltkk

#pragma once
#include <cstdint>
#include <linux/fb.h>

namespace okou {
// Validate the complete visible viewport before the LVGL fbdev adapter maps it.
inline bool compatible_framebuffer(const fb_var_screeninfo &v, const fb_fix_screeninfo &f) {
    if (v.xres != 320 || v.yres != 170 || v.bits_per_pixel != 16 ||
        v.red.offset != 11 || v.red.length != 5 || v.red.msb_right ||
        v.green.offset != 5 || v.green.length != 6 || v.green.msb_right ||
        v.blue.offset != 0 || v.blue.length != 5 || v.blue.msb_right ||
        v.transp.length || v.nonstd || v.grayscale ||
        f.type != FB_TYPE_PACKED_PIXELS || f.visual != FB_VISUAL_TRUECOLOR) return false;
    const uint64_t right = uint64_t(v.xoffset) + v.xres;
    const uint64_t bottom = uint64_t(v.yoffset) + v.yres;
    return right <= v.xres_virtual && bottom <= v.yres_virtual &&
           right * 2 <= f.line_length &&
           (bottom - 1) * f.line_length + right * 2 <= f.smem_len;
}
} // namespace okou

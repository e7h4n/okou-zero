#pragma once
#include <cstdint>
#include <linux/fb.h>

namespace okou {
inline bool compatible_framebuffer(const fb_var_screeninfo &variable,
                                   const fb_fix_screeninfo &fixed) {
    if (variable.xres != 320 || variable.yres != 170 || variable.bits_per_pixel != 16 ||
        variable.red.offset != 11 || variable.red.length != 5 || variable.red.msb_right != 0 ||
        variable.green.offset != 5 || variable.green.length != 6 || variable.green.msb_right != 0 ||
        variable.blue.offset != 0 || variable.blue.length != 5 || variable.blue.msb_right != 0)
        return false;
    const auto end_x = static_cast<std::uint64_t>(variable.xoffset) + 320;
    const auto end_y = static_cast<std::uint64_t>(variable.yoffset) + 170;
    return end_x <= variable.xres_virtual && end_y <= variable.yres_virtual &&
           end_x * 2 <= fixed.line_length &&
           (end_y - 1) * fixed.line_length + end_x * 2 <= fixed.smem_len;
}
} // namespace okou

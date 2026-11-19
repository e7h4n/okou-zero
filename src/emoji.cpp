// SPDX-License-Identifier: MIT
#include "emoji.hpp"
#include <algorithm>
#include <array>
#include <ft2build.h>
#include FT_FREETYPE_H
#include "src/misc/cache/instance/lv_image_cache.h"
#include <hb-ft.h>

namespace okou {
namespace {
struct Rune {
    std::uint32_t value;
    int end;
};
Rune rune(const std::string &text, int start) {
    if (start >= static_cast<int>(text.size()))
        return {0, start};
    auto end = static_cast<std::uint32_t>(start);
    const auto value = lv_text_encoded_next(text.c_str(), &end);
    return {value, std::max(start + 1, static_cast<int>(end))};
}
// Unicode Extended_Pictographic intervals, the same property used by Web.
constexpr std::pair<std::uint32_t, std::uint32_t> pictographic_ranges[] = {
    {0xa9, 0xa9},       {0xae, 0xae},       {0x203c, 0x203c},   {0x2049, 0x2049},
    {0x2122, 0x2122},   {0x2139, 0x2139},   {0x2194, 0x2199},   {0x21a9, 0x21aa},
    {0x231a, 0x231b},   {0x2328, 0x2328},   {0x23cf, 0x23cf},   {0x23e9, 0x23f3},
    {0x23f8, 0x23fa},   {0x24c2, 0x24c2},   {0x25aa, 0x25ab},   {0x25b6, 0x25b6},
    {0x25c0, 0x25c0},   {0x25fb, 0x25fe},   {0x2600, 0x2604},   {0x260e, 0x260e},
    {0x2611, 0x2611},   {0x2614, 0x2615},   {0x2618, 0x2618},   {0x261d, 0x261d},
    {0x2620, 0x2620},   {0x2622, 0x2623},   {0x2626, 0x2626},   {0x262a, 0x262a},
    {0x262e, 0x262f},   {0x2638, 0x263a},   {0x2640, 0x2640},   {0x2642, 0x2642},
    {0x2648, 0x2653},   {0x265f, 0x2660},   {0x2663, 0x2663},   {0x2665, 0x2666},
    {0x2668, 0x2668},   {0x267b, 0x267b},   {0x267e, 0x267f},   {0x2692, 0x2697},
    {0x2699, 0x2699},   {0x269b, 0x269c},   {0x26a0, 0x26a1},   {0x26a7, 0x26a7},
    {0x26aa, 0x26ab},   {0x26b0, 0x26b1},   {0x26bd, 0x26be},   {0x26c4, 0x26c5},
    {0x26c8, 0x26c8},   {0x26ce, 0x26cf},   {0x26d1, 0x26d1},   {0x26d3, 0x26d4},
    {0x26e9, 0x26ea},   {0x26f0, 0x26f5},   {0x26f7, 0x26fa},   {0x26fd, 0x26fd},
    {0x2702, 0x2702},   {0x2705, 0x2705},   {0x2708, 0x270d},   {0x270f, 0x270f},
    {0x2712, 0x2712},   {0x2714, 0x2714},   {0x2716, 0x2716},   {0x271d, 0x271d},
    {0x2721, 0x2721},   {0x2728, 0x2728},   {0x2733, 0x2734},   {0x2744, 0x2744},
    {0x2747, 0x2747},   {0x274c, 0x274c},   {0x274e, 0x274e},   {0x2753, 0x2755},
    {0x2757, 0x2757},   {0x2763, 0x2764},   {0x2795, 0x2797},   {0x27a1, 0x27a1},
    {0x27b0, 0x27b0},   {0x27bf, 0x27bf},   {0x2934, 0x2935},   {0x2b05, 0x2b07},
    {0x2b1b, 0x2b1c},   {0x2b50, 0x2b50},   {0x2b55, 0x2b55},   {0x3030, 0x3030},
    {0x303d, 0x303d},   {0x3297, 0x3297},   {0x3299, 0x3299},   {0x1f004, 0x1f004},
    {0x1f02c, 0x1f02f}, {0x1f094, 0x1f09f}, {0x1f0af, 0x1f0b0}, {0x1f0c0, 0x1f0c0},
    {0x1f0cf, 0x1f0d0}, {0x1f0f6, 0x1f0ff}, {0x1f170, 0x1f171}, {0x1f17e, 0x1f17f},
    {0x1f18e, 0x1f18e}, {0x1f191, 0x1f19a}, {0x1f1ae, 0x1f1e5}, {0x1f201, 0x1f20f},
    {0x1f21a, 0x1f21a}, {0x1f22f, 0x1f22f}, {0x1f232, 0x1f23a}, {0x1f23c, 0x1f23f},
    {0x1f249, 0x1f25f}, {0x1f266, 0x1f321}, {0x1f324, 0x1f393}, {0x1f396, 0x1f397},
    {0x1f399, 0x1f39b}, {0x1f39e, 0x1f3f0}, {0x1f3f3, 0x1f3f5}, {0x1f3f7, 0x1f3fa},
    {0x1f400, 0x1f4fd}, {0x1f4ff, 0x1f53d}, {0x1f549, 0x1f54e}, {0x1f550, 0x1f567},
    {0x1f56f, 0x1f570}, {0x1f573, 0x1f57a}, {0x1f587, 0x1f587}, {0x1f58a, 0x1f58d},
    {0x1f590, 0x1f590}, {0x1f595, 0x1f596}, {0x1f5a4, 0x1f5a5}, {0x1f5a8, 0x1f5a8},
    {0x1f5b1, 0x1f5b2}, {0x1f5bc, 0x1f5bc}, {0x1f5c2, 0x1f5c4}, {0x1f5d1, 0x1f5d3},
    {0x1f5dc, 0x1f5de}, {0x1f5e1, 0x1f5e1}, {0x1f5e3, 0x1f5e3}, {0x1f5e8, 0x1f5e8},
    {0x1f5ef, 0x1f5ef}, {0x1f5f3, 0x1f5f3}, {0x1f5fa, 0x1f64f}, {0x1f680, 0x1f6c5},
    {0x1f6cb, 0x1f6d2}, {0x1f6d5, 0x1f6e5}, {0x1f6e9, 0x1f6e9}, {0x1f6eb, 0x1f6f0},
    {0x1f6f3, 0x1f6ff}, {0x1f7da, 0x1f7ff}, {0x1f80c, 0x1f80f}, {0x1f848, 0x1f84f},
    {0x1f85a, 0x1f85f}, {0x1f888, 0x1f88f}, {0x1f8ae, 0x1f8af}, {0x1f8bc, 0x1f8bf},
    {0x1f8c2, 0x1f8cf}, {0x1f8d9, 0x1f8ff}, {0x1f90c, 0x1f93a}, {0x1f93c, 0x1f945},
    {0x1f947, 0x1f9ff}, {0x1fa58, 0x1fa5f}, {0x1fa6e, 0x1faff}, {0x1fc00, 0x1fffd},
};
bool flag_letter(std::uint32_t c) {
    return c >= 0x1f1e6 && c <= 0x1f1ff;
}
bool pictograph(std::uint32_t c) {
    const auto end = std::end(pictographic_ranges);
    const auto range = std::lower_bound(std::begin(pictographic_ranges), end, c,
                                        [](const auto &r, auto value) { return r.second < value; });
    return range != end && c >= range->first;
}
bool variation(std::uint32_t c) {
    return c == 0xfe0e || c == 0xfe0f;
}
bool whitespace(std::uint32_t c) {
    return (c >= 9 && c <= 13) || c == 0x20 || c == 0xa0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200a) || c == 0x2028 || c == 0x2029 || c == 0x202f ||
           c == 0x205f || c == 0x3000 || c == 0xfeff;
}
int pictograph_end(const std::string &text, int end) {
    auto next = rune(text, end);
    if (variation(next.value)) {
        end = next.end;
        next = rune(text, end);
    }
    if (next.value >= 0x1f3fb && next.value <= 0x1f3ff)
        end = next.end;
    return end;
}
int emoji_end(const std::string &text, Rune first) {
    auto next = rune(text, first.end);
    if (flag_letter(first.value) && flag_letter(next.value))
        return next.end;
    if (first.value == '#' || first.value == '*' || (first.value >= '0' && first.value <= '9')) {
        if (next.value == 0xfe0f)
            next = rune(text, next.end);
        return next.value == 0x20e3 ? next.end : 0;
    }
    if (!pictograph(first.value))
        return 0;
    int end = pictograph_end(text, first.end);
    while ((next = rune(text, end)).value == 0x200d) {
        auto joined = rune(text, next.end);
        if (!pictograph(joined.value))
            break;
        end = pictograph_end(text, joined.end);
    }
    return end;
}
std::string trim(std::string text) {
    int first = 0, last = 0;
    bool started = false;
    for (int offset = 0; offset < static_cast<int>(text.size());) {
        const auto current = rune(text, offset);
        if (!whitespace(current.value)) {
            if (!started)
                first = offset;
            started = true;
            last = current.end;
        }
        offset = current.end;
    }
    return text.substr(first, last - first);
}
constexpr int max_size = 18;
struct Glyph {
    lv_image_dsc_t image{};
    std::array<std::uint8_t, max_size * max_size * 4> pixels{};
};
void delete_glyph(lv_event_t *event) {
    auto *glyph = static_cast<Glyph *>(lv_event_get_user_data(event));
    lv_image_cache_drop(&glyph->image);
    delete glyph;
}
} // namespace

std::pair<std::string, std::string> split_emoji_title(const std::string &title) {
    auto text = trim(title);
    for (int offset = 0; offset < static_cast<int>(text.size());) {
        const auto first = rune(text, offset);
        const auto end = emoji_end(text, first);
        if (end) {
            auto before = trim(text.substr(0, offset));
            auto after = trim(text.substr(end));
            if (!before.empty() && !after.empty())
                before += ' ';
            return {text.substr(offset, end - offset), before + after};
        }
        offset = first.end;
    }
    return {{}, std::move(text)};
}

struct EmojiRenderer::Font {
    FT_Library library = nullptr;
    FT_Face face = nullptr;
    hb_font_t *shaper = nullptr;

    Font() {
        if (FT_Init_FreeType(&library) == 0 &&
            FT_New_Face(library, "/usr/share/fonts/truetype/noto/NotoColorEmoji.ttf", 0, &face) ==
                0 &&
            FT_Select_Size(face, 0) == 0)
            shaper = hb_ft_font_create_referenced(face);
    }
    ~Font() {
        if (shaper)
            hb_font_destroy(shaper);
        if (face)
            FT_Done_Face(face);
        if (library)
            FT_Done_FreeType(library);
    }
};
EmojiRenderer::EmojiRenderer() : font_(std::make_unique<Font>()) {}
EmojiRenderer::~EmojiRenderer() = default;

bool EmojiRenderer::draw(lv_obj_t *parent, const std::string &emoji, int x, int y, int size) {
    if (!font_->shaper || emoji.empty() || size <= 0 || size > max_size)
        return false;
    std::unique_ptr<hb_buffer_t, decltype(&hb_buffer_destroy)> buffer(hb_buffer_create(),
                                                                      hb_buffer_destroy);
    hb_buffer_add_utf8(buffer.get(), emoji.c_str(), -1, 0, -1);
    hb_buffer_guess_segment_properties(buffer.get());
    hb_shape(font_->shaper, buffer.get(), nullptr, 0);
    unsigned count = 0;
    const auto *shaped = hb_buffer_get_glyph_infos(buffer.get(), &count);
    if (count != 1 || !shaped[0].codepoint ||
        FT_Load_Glyph(font_->face, shaped[0].codepoint, FT_LOAD_COLOR) != 0)
        return false;
    const auto &bitmap = font_->face->glyph->bitmap;
    if (bitmap.pixel_mode != FT_PIXEL_MODE_BGRA || !bitmap.width || !bitmap.rows ||
        bitmap.pitch <= 0)
        return false;

    auto glyph = std::make_unique<Glyph>();
    const int sw = static_cast<int>(bitmap.width), sh = static_cast<int>(bitmap.rows);
    const int longest = std::max(sw, sh);
    const int width = std::clamp(sw * size / longest, 1, sw);
    const int height = std::clamp(sh * size / longest, 1, sh);
    // FreeType delivers premultiplied BGRA. Average in that space, then
    // convert to the straight-alpha byte layout used by LVGL ARGB8888.
    for (int dy = 0; dy < height; ++dy) {
        for (int dx = 0; dx < width; ++dx) {
            std::array<unsigned, 4> sum{};
            unsigned samples = 0;
            for (int sy = dy * sh / height; sy < (dy + 1) * sh / height; ++sy) {
                for (int sx = dx * sw / width; sx < (dx + 1) * sw / width; ++sx) {
                    const auto *src = bitmap.buffer + sy * bitmap.pitch + 4 * sx;
                    for (int c = 0; c < 4; ++c)
                        sum[c] += src[c];
                    ++samples;
                }
            }
            auto *dst = glyph->pixels.data() + 4 * (dy * width + dx);
            dst[3] = static_cast<std::uint8_t>(sum[3] / samples);
            if (sum[3] != 0)
                for (int c = 0; c < 3; ++c)
                    dst[c] = static_cast<std::uint8_t>(std::min(255U, 255 * sum[c] / sum[3]));
        }
    }
    auto &image = glyph->image;
    image.header.magic = LV_IMAGE_HEADER_MAGIC;
    image.header.cf = LV_COLOR_FORMAT_ARGB8888;
    image.header.w = width;
    image.header.h = height;
    image.header.stride = width * 4;
    image.data = glyph->pixels.data();
    image.data_size = width * height * 4;
    auto *object = lv_image_create(parent);
    lv_obj_set_pos(object, x + (size - width) / 2, y + (size - height) / 2);
    lv_image_set_src(object, &image);
    lv_obj_add_event_cb(object, delete_glyph, LV_EVENT_DELETE, glyph.release());
    return true;
}
} // namespace okou

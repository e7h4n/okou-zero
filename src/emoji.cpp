// SPDX-License-Identifier: MIT
#include "emoji.hpp"
#include <algorithm>
#include <array>
#include <ft2build.h>
#include FT_FREETYPE_H
#include "src/misc/cache/instance/lv_image_cache.h"
#include <hb-ft.h>
#include <unicode/uchar.h>
#include <unicode/utf8.h>

namespace okou {
namespace {
struct Rune {
    UChar32 value;
    int end;
};
Rune rune(const std::string &text, int start) {
    if (start >= static_cast<int>(text.size()))
        return {-1, start};
    UChar32 value;
    auto end = start;
    U8_NEXT(reinterpret_cast<const std::uint8_t *>(text.data()), end, static_cast<int>(text.size()),
            value);
    return {value, end};
}
bool flag_letter(UChar32 c) {
    return c >= 0x1f1e6 && c <= 0x1f1ff;
}
bool pictograph(UChar32 c) {
    return c >= 0 && u_hasBinaryProperty(c, UCHAR_EXTENDED_PICTOGRAPHIC);
}
bool variation(UChar32 c) {
    return c == 0xfe0e || c == 0xfe0f;
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
        if (!u_isUWhiteSpace(current.value)) {
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

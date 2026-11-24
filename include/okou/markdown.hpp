#pragma once
#include "okou/platform.hpp"
#include <string>

namespace okou {
struct MarkdownFonts {
    lv_font_t *bold = nullptr, *italic = nullptr, *heading = nullptr, *code = nullptr;
};

// Returns a single, measured message body for scrolling and voice hit testing.
lv_obj_t *markdown_body(lv_obj_t *parent, const std::string &source, int x, int y, int width,
                        const MarkdownFonts &fonts, std::string *pinned_heading = nullptr);
} // namespace okou

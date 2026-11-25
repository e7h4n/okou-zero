#pragma once
#include "okou/platform.hpp"
#include <string>

namespace okou {
struct MarkdownFonts {
    lv_font_t *bold = nullptr, *italic = nullptr, *heading = nullptr, *code = nullptr;
};

// The folded reader pins a leading heading when it fits on one line.
lv_obj_t *markdown_body(lv_obj_t *parent, const std::string &source, int width,
                        const MarkdownFonts &fonts, std::string &pinned_heading);
} // namespace okou

#include "okou/markdown.hpp"
#include <md4c.h>
extern "C" {
#include <entity.h>
}
#include <algorithm>
#include <charconv>
#include <codecvt>
#include <locale>
#include <string_view>
#include <vector>

namespace okou {
namespace {
enum Style : unsigned { bold = 1, italic = 2, link = 4, code = 8, strike = 16 };
struct Run {
    std::string text;
    unsigned style;
};
using Cell = std::vector<Run>;
enum class Kind { paragraph, heading, code, rule, table };
struct Block {
    Kind kind;
    int indent;
    bool quoted;
    std::vector<Cell> cells;
};
struct List {
    bool ordered;
    unsigned next;
};

struct Document {
    std::vector<Block> blocks;
    std::vector<List> lists;
    std::vector<unsigned> styles{0};
    std::string marker;
    int quotes = 0;
    bool active = false;
    bool header_cell = false;
    size_t objects = 0;

    void start(Kind kind) {
        blocks.push_back({kind,
                          std::min(48, (quotes + static_cast<int>(lists.size())) * 8),
                          quotes > 0,
                          {Cell{}}});
        ++objects;
        active = true;
        if (!marker.empty()) {
            blocks.back().cells.front().push_back({marker, 0});
            marker.clear();
        }
    }
    void append(std::string_view text) {
        if (text.empty())
            return;
        if (!active)
            start(Kind::paragraph);
        auto &cell = blocks.back().cells.back();
        const unsigned style = styles.back() | (header_cell ? unsigned(bold) : 0u);
        if (cell.empty() || cell.back().style != style) {
            cell.push_back({{}, style});
            ++objects;
        }
        cell.back().text.append(text);
    }
};

int enter_block(MD_BLOCKTYPE type, void *detail, void *data) {
    auto &d = *static_cast<Document *>(data);
    switch (type) {
    case MD_BLOCK_QUOTE:
        ++d.quotes;
        d.active = false;
        break;
    case MD_BLOCK_UL:
        d.lists.push_back({false, 0});
        break;
    case MD_BLOCK_OL:
        d.lists.push_back({true, static_cast<MD_BLOCK_OL_DETAIL *>(detail)->start});
        break;
    case MD_BLOCK_LI: {
        const auto &item = *static_cast<MD_BLOCK_LI_DETAIL *>(detail);
        auto &list = d.lists.back();
        d.marker = list.ordered ? std::to_string(list.next++) + ". " : "• ";
        if (item.is_task)
            d.marker += item.task_mark == ' ' ? "☐ " : "☑ ";
        d.active = false;
        break;
    }
    case MD_BLOCK_H:
        d.start(Kind::heading);
        break;
    case MD_BLOCK_HR:
        d.start(Kind::rule);
        break;
    case MD_BLOCK_CODE:
        d.start(Kind::code);
        break;
    case MD_BLOCK_P:
        d.start(Kind::paragraph);
        break;
    case MD_BLOCK_TR:
        d.start(Kind::table);
        d.blocks.back().cells.clear();
        break;
    case MD_BLOCK_TH:
    case MD_BLOCK_TD:
        d.blocks.back().cells.emplace_back();
        ++d.objects;
        d.header_cell = type == MD_BLOCK_TH;
        break;
    default:
        break;
    }
    return d.objects > 2048;
}
int leave_block(MD_BLOCKTYPE type, void *, void *data) {
    auto &d = *static_cast<Document *>(data);
    if (type == MD_BLOCK_QUOTE)
        --d.quotes;
    if (type == MD_BLOCK_UL || type == MD_BLOCK_OL)
        d.lists.pop_back();
    if (type == MD_BLOCK_TH || type == MD_BLOCK_TD) {
        d.header_cell = false;
    } else {
        if (type == MD_BLOCK_LI && !d.marker.empty())
            d.start(Kind::paragraph);
        d.active = false;
    }
    return 0;
}
int enter_span(MD_SPANTYPE type, void *, void *data) {
    auto &styles = static_cast<Document *>(data)->styles;
    unsigned flag = 0;
    switch (type) {
    case MD_SPAN_EM:
        flag = italic;
        break;
    case MD_SPAN_STRONG:
        flag = bold;
        break;
    case MD_SPAN_A:
        flag = link;
        break;
    case MD_SPAN_CODE:
        flag = code;
        break;
    case MD_SPAN_DEL:
        flag = strike;
        break;
    default:
        break;
    }
    styles.push_back(styles.back() | flag);
    return 0;
}
int leave_span(MD_SPANTYPE, void *, void *data) {
    static_cast<Document *>(data)->styles.pop_back();
    return 0;
}
void append_codepoint(Document &d, unsigned cp) {
    if (!cp || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
        cp = 0xfffd;
    d.append(std::wstring_convert<std::codecvt_utf8<char32_t>, char32_t>{}.to_bytes(
        static_cast<char32_t>(cp)));
}
int text(MD_TEXTTYPE type, const char *value, MD_SIZE len, void *data) {
    auto &d = *static_cast<Document *>(data);
    std::string_view s(value, len);
    if (type == MD_TEXT_ENTITY && s.size() > 3 && s[1] == '#') {
        bool hex = s[2] == 'x' || s[2] == 'X';
        unsigned cp = 0;
        std::from_chars(s.data() + (hex ? 3 : 2), s.data() + s.size() - 1, cp, hex ? 16 : 10);
        append_codepoint(d, cp);
    } else if (type == MD_TEXT_ENTITY) {
        if (auto *e = entity_lookup(value, len)) {
            append_codepoint(d, e->codepoints[0]);
            if (e->codepoints[1])
                append_codepoint(d, e->codepoints[1]);
        } else
            d.append(s);
    } else if (type == MD_TEXT_NULLCHAR)
        d.append("�");
    else
        d.append(type == MD_TEXT_BR || type == MD_TEXT_SOFTBR ? "\n" : s);
    return d.objects > 2048;
}

void shade(lv_obj_t *object, uint32_t color) {
    lv_obj_set_style_bg_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
}
int render_cell(lv_obj_t *body, const Cell &cell, const Block &block, int x, int y, int width,
                const MarkdownFonts &fonts) {
    auto *group = lv_spangroup_create(body);
    lv_obj_remove_style_all(group);
    lv_obj_set_pos(group, x, y);
    bool pre = block.kind == Kind::code;
    if (pre)
        shade(group, 0x263735);
    for (const auto &run : cell) {
        auto *span = lv_spangroup_add_span(group);
        lv_span_set_text(span, run.text.c_str());
        auto *style = lv_span_get_style(span);
        const auto *font = block.kind == Kind::heading ? fonts.heading
                           : pre || (run.style & code) ? fonts.code
                           : (run.style & bold)        ? fonts.bold
                           : (run.style & italic)      ? fonts.italic
                                                       : nullptr;
        if (font)
            lv_style_set_text_font(style, font);
        if (run.style & link)
            lv_style_set_text_color(style, lv_color_hex(0x9ef1ce));
        if (run.style & code)
            lv_style_set_text_color(style, lv_color_hex(0xf2bb79));
        unsigned decor = ((run.style & link) ? LV_TEXT_DECOR_UNDERLINE : 0) |
                         ((run.style & strike) ? LV_TEXT_DECOR_STRIKETHROUGH : 0);
        if (decor)
            lv_style_set_text_decor(style, static_cast<lv_text_decor_t>(decor));
    }
    const int pad = pre ? 4 : 0;
    lv_obj_set_style_pad_hor(group, pad, 0);
    const int height =
        std::max<int>(lv_font_get_line_height(lv_obj_get_style_text_font(group, LV_PART_MAIN)),
                      lv_spangroup_get_expand_height(group, width - 2 * pad));
    lv_obj_set_size(group, width, height);
    return height;
}
} // namespace

lv_obj_t *markdown_body(lv_obj_t *parent, const std::string &source, int x, int y, int width,
                        const MarkdownFonts &fonts, std::string *pinned_heading) {
    if (pinned_heading)
        pinned_heading->clear();
    Document doc;
    const MD_PARSER parser{0,           MD_DIALECT_GITHUB | MD_FLAG_NOHTML,
                           enter_block, leave_block,
                           enter_span,  leave_span,
                           text,        nullptr,
                           nullptr};
    if (md_parse(source.data(), source.size(), &parser, &doc) != 0 || doc.blocks.empty() ||
        (doc.blocks.size() == 1 && doc.blocks.front().kind == Kind::paragraph &&
         !doc.blocks.front().indent && doc.blocks.front().cells.front().size() == 1 &&
         doc.blocks.front().cells.front().front().text == source &&
         !doc.blocks.front().cells.front().front().style)) {
        auto *label = lv_label_create(parent);
        lv_obj_set_pos(label, x, y);
        lv_obj_set_width(label, width);
        lv_label_set_text(label, source.c_str());
        return label;
    }
    auto *body = lv_obj_create(parent);
    lv_obj_remove_style_all(body);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(body, x, y);
    if (pinned_heading && doc.blocks.front().kind == Kind::heading && fonts.heading) {
        std::string title;
        for (const auto &run : doc.blocks.front().cells.front())
            title += run.text;
        lv_point_t size{};
        lv_text_get_size(&size, title.c_str(), fonts.heading, 0, 0, LV_COORD_MAX,
                         LV_TEXT_FLAG_NONE);
        if (size.x <= width && title.find('\n') == std::string::npos) {
            *pinned_heading = std::move(title);
            doc.blocks.erase(doc.blocks.begin());
        }
    }
    int bottom = 0;
    for (const auto &block : doc.blocks) {
        int left = block.indent;
        int top = bottom;
        if (block.kind == Kind::rule) {
            auto *rule = lv_obj_create(body);
            lv_obj_remove_style_all(rule);
            lv_obj_set_pos(rule, left, top);
            lv_obj_set_size(rule, width - left, 1);
            shade(rule, 0xbcb5ae);
            ++bottom;
        } else {
            // Stack wide rows rather than squeezing cells into unreadable columns.
            int cols = block.cells.size() > 3 ? 1 : static_cast<int>(block.cells.size());
            for (int i = 0; i < static_cast<int>(block.cells.size()); ++i) {
                if (i && i % cols == 0)
                    top = bottom;
                int step = (width - left) / cols;
                bottom = std::max(bottom, top + render_cell(body, block.cells[i], block,
                                                            left + (i % cols) * step, top,
                                                            step - (cols > 1 ? 4 : 0), fonts));
            }
        }
        if (block.quoted) {
            auto *bar = lv_obj_create(body);
            lv_obj_remove_style_all(bar);
            lv_obj_set_pos(bar, left - 6, top);
            lv_obj_set_size(bar, 2, bottom - top);
            shade(bar, 0xab9b92);
        }
        bottom += 3;
    }
    lv_obj_set_size(body, width, std::max(0, bottom - 3));
    return body;
}
} // namespace okou

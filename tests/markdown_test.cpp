#include "okou/markdown.hpp"
#include <cstdlib>
#include <string>

static void check(bool condition) {
    if (!condition)
        std::abort();
}
static std::string contents(lv_obj_t *group) {
    std::string value;
    for (uint32_t i = 0; i < lv_spangroup_get_span_count(group); ++i)
        value += lv_span_get_text(lv_spangroup_get_child(group, i));
    return value;
}
int main() {
    lv_init();
    auto *display = lv_display_create(320, 170);
    auto *screen = lv_display_get_screen_active(display);
    auto strong_font = lv_font_montserrat_16;
    okou::MarkdownFonts fonts;
    fonts.bold = &strong_font;
    auto *body =
        okou::markdown_body(screen,
                            "## 标题\n\n3. **粗体** and [link](https://example.org)\n"
                            "   - `a_b` &amp; &#x4e2d;\n\n> *引用*\n\n"
                            "```cpp\n**literal**\n  x = 1;\n```\n\n"
                            "| Key | Meaning |\n| --- | --- |\n| ID | A long value that should "
                            "wrap onto multiple lines when displayed in a narrow table cell |",
                            0, 0, 299, fonts);
    lv_obj_update_layout(body);
    auto child = [&](int i) { return lv_obj_get_child(body, i); };
    check(contents(child(0)) == "标题");
    check(contents(child(1)) == "3. 粗体 and link");
    check(contents(child(2)) == "• a_b & 中");
    check(lv_obj_get_x(child(2)) > lv_obj_get_x(child(1)));
    check(contents(child(3)) == "引用");
    check(contents(child(5)).find("**literal**\n  x = 1;") == 0);
    lv_style_value_t style;
    auto *bold = lv_spangroup_get_child(child(1), 1);
    check(lv_style_get_prop(lv_span_get_style(bold), LV_STYLE_TEXT_FONT, &style) ==
              LV_STYLE_RES_FOUND &&
          style.ptr == fonts.bold);
    auto *link = lv_spangroup_get_child(child(1), 3);
    check(lv_style_get_prop(lv_span_get_style(link), LV_STYLE_TEXT_DECOR, &style) ==
              LV_STYLE_RES_FOUND &&
          (style.num & LV_TEXT_DECOR_UNDERLINE));
    check(contents(child(6)) == "Key" && contents(child(7)) == "Meaning");
    check(lv_obj_get_y(child(8)) == lv_obj_get_y(child(9)) && lv_obj_get_x(child(9)) > 0);
    check(lv_obj_get_height(child(9)) > lv_obj_get_height(child(8)));
    check(lv_obj_get_height(body) >= lv_obj_get_y(child(9)) + lv_obj_get_height(child(9)));
    auto *voice = okou::markdown_body(screen, "[Voice message]", 0, 0, 299, fonts);
    check(lv_obj_has_class(voice, &lv_label_class));
    check(std::string(lv_label_get_text(voice)) == "[Voice message]");
    lv_display_delete(display);
    lv_deinit();
}

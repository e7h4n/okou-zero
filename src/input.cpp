#include "okou/platform.hpp"
#include "update_process.hpp"
#include <cerrno>
#include <fcntl.h>
#include <linux/input.h>
#include <stdexcept>
#include <string_view>
#include <unistd.h>
#if OKOU_ZERO_DESKTOP
#include <SDL2/SDL.h>
#endif

namespace okou {
void erase_utf8(std::string &text) {
    if (text.empty())
        return;
    auto pos = text.size() - 1;
    while (pos > 0 && (static_cast<unsigned char>(text[pos]) & 0xc0) == 0x80)
        --pos;
    text.erase(pos);
}
Input map_key(std::uint16_t code, bool shift, bool caps, bool ctrl) {
    Input out;
    out.shift = shift;
    out.ctrl = ctrl;
    switch (code) {
    case KEY_VOLUMEUP:
        out.key = Key::volume_up;
        return out;
    case KEY_VOLUMEDOWN:
        out.key = Key::volume_down;
        return out;
    case KEY_MUTE:
        out.key = Key::mute;
        return out;
    case KEY_SPACE:
        if (!ctrl) {
            out.key = Key::space;
            out.text = " ";
        }
        return out;
    case KEY_ENTER:
        out.key = Key::enter;
        return out;
    case KEY_BACKSPACE:
        out.key = Key::backspace;
        return out;
    case KEY_ESC:
        out.key = Key::escape;
        return out;
    case KEY_UP:
        out.key = Key::up;
        return out;
    case KEY_DOWN:
        out.key = Key::down;
        return out;
    case KEY_LEFT:
        out.key = Key::left;
        return out;
    case KEY_RIGHT:
        out.key = Key::right;
        return out;
    case KEY_TAB:
        out.key = Key::tab;
        return out;
    case KEY_PAGEUP:
        out.key = Key::page_up;
        return out;
    case KEY_PAGEDOWN:
        out.key = Key::page_down;
        return out;
    default:
        break;
    }
    struct Pair {
        int code;
        char plain;
        char shifted;
    };
    static const Pair punctuation[] = {
        {KEY_1, '1', '!'},         {KEY_2, '2', '@'},           {KEY_3, '3', '#'},
        {KEY_4, '4', '$'},         {KEY_5, '5', '%'},           {KEY_6, '6', '^'},
        {KEY_7, '7', '&'},         {KEY_8, '8', '*'},           {KEY_9, '9', '('},
        {KEY_0, '0', ')'},         {KEY_MINUS, '-', '_'},       {KEY_EQUAL, '=', '+'},
        {KEY_LEFTBRACE, '[', '{'}, {KEY_RIGHTBRACE, ']', '}'},  {KEY_BACKSLASH, '\\', '|'},
        {KEY_SEMICOLON, ';', ':'}, {KEY_APOSTROPHE, '\'', '"'}, {KEY_GRAVE, '`', '~'},
        {KEY_COMMA, ',', '<'},     {KEY_DOT, '.', '>'},         {KEY_SLASH, '/', '?'}};
    static const Pair letters[] = {
        {KEY_A, 'a', 'A'}, {KEY_B, 'b', 'B'}, {KEY_C, 'c', 'C'}, {KEY_D, 'd', 'D'},
        {KEY_E, 'e', 'E'}, {KEY_F, 'f', 'F'}, {KEY_G, 'g', 'G'}, {KEY_H, 'h', 'H'},
        {KEY_I, 'i', 'I'}, {KEY_J, 'j', 'J'}, {KEY_K, 'k', 'K'}, {KEY_L, 'l', 'L'},
        {KEY_M, 'm', 'M'}, {KEY_N, 'n', 'N'}, {KEY_O, 'o', 'O'}, {KEY_P, 'p', 'P'},
        {KEY_Q, 'q', 'Q'}, {KEY_R, 'r', 'R'}, {KEY_S, 's', 'S'}, {KEY_T, 't', 'T'},
        {KEY_U, 'u', 'U'}, {KEY_V, 'v', 'V'}, {KEY_W, 'w', 'W'}, {KEY_X, 'x', 'X'},
        {KEY_Y, 'y', 'Y'}, {KEY_Z, 'z', 'Z'}};
    if (ctrl) {
        switch (code) {
        case KEY_R:
            out.key = Key::record;
            break;
        case KEY_W:
            out.key = Key::workspace;
            break;
        case KEY_L:
            out.key = Key::reload;
            break;
        case KEY_Q:
            out.key = Key::quit;
            break;
        default:
            break;
        }
        return out;
    }
    for (auto entry : letters)
        if (code == entry.code) {
            out.key = Key::text;
            out.text = shift != caps ? entry.shifted : entry.plain;
            return out;
        }
    for (auto entry : punctuation)
        if (code == entry.code) {
            out.key = Key::text;
            out.text = shift ? entry.shifted : entry.plain;
            return out;
        }
    return out;
}
Keyboard::Keyboard(bool desktop, const std::string &path)
    : desktop_(desktop), update_shortcut_(std::make_unique<UpdateShortcut>()) {
    if (!desktop_) {
        fd_ = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd_ < 0)
            throw std::runtime_error("Cannot open keyboard; check input-group access");
    }
#if OKOU_ZERO_DESKTOP
    else
        SDL_StartTextInput();
#endif
}
Keyboard::~Keyboard() {
    if (fd_ >= 0)
        close(fd_);
#if OKOU_ZERO_DESKTOP
    if (desktop_)
        SDL_StopTextInput();
#endif
}
std::vector<Input> Keyboard::poll() {
    std::vector<Input> result;
#if OKOU_ZERO_DESKTOP
    if (desktop_) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                result.push_back({Key::quit, {}, false, false});
                continue;
            }
            if (event.type == SDL_TEXTINPUT) {
                if (std::string_view(event.text.text) != " ")
                    result.push_back({Key::text, event.text.text, false, false});
                continue;
            }
            if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_FOCUS_LOST)
                result.push_back({Key::enter_cancel, {}});
            if (event.type == SDL_KEYUP &&
                (event.key.keysym.sym == SDLK_RETURN || event.key.keysym.sym == SDLK_KP_ENTER))
                result.push_back({Key::enter_up, {}});
            if (event.type != SDL_KEYDOWN)
                continue;
            auto mod = event.key.keysym.mod;
            Input out;
            out.shift = (mod & KMOD_SHIFT) != 0;
            out.ctrl = (mod & KMOD_CTRL) != 0;
            if (event.key.keysym.sym == SDLK_u && out.ctrl && (mod & KMOD_ALT) &&
                !event.key.repeat) {
                result.push_back({Key::update, {}, false, true});
                continue;
            }
            switch (event.key.keysym.sym) {
            case SDLK_RETURN:
            case SDLK_KP_ENTER:
                if (!event.key.repeat)
                    out.key = Key::enter_down;
                break;
            case SDLK_VOLUMEUP:
                out.key = Key::volume_up;
                break;
            case SDLK_VOLUMEDOWN:
                out.key = Key::volume_down;
                break;
            case SDLK_AUDIOMUTE:
            case SDLK_MUTE:
                if (!event.key.repeat)
                    out.key = Key::mute;
                break;
            case SDLK_SPACE:
                if (!event.key.repeat && !out.ctrl) {
                    out.key = Key::space;
                    out.text = " ";
                }
                break;
            case SDLK_ESCAPE:
                out.key = Key::escape;
                break;
            case SDLK_BACKSPACE:
                out.key = Key::backspace;
                break;
            case SDLK_UP:
                out.key = Key::up;
                break;
            case SDLK_DOWN:
                out.key = Key::down;
                break;
            case SDLK_LEFT:
                out.key = Key::left;
                break;
            case SDLK_RIGHT:
                out.key = Key::right;
                break;
            case SDLK_TAB:
                out.key = Key::tab;
                break;
            case SDLK_PAGEUP:
                out.key = Key::page_up;
                break;
            case SDLK_PAGEDOWN:
                out.key = Key::page_down;
                break;
            case SDLK_r:
                if (out.ctrl)
                    out.key = Key::record;
                break;
            case SDLK_w:
                if (out.ctrl)
                    out.key = Key::workspace;
                break;
            case SDLK_l:
                if (out.ctrl)
                    out.key = Key::reload;
                break;
            case SDLK_q:
                if (out.ctrl)
                    out.key = Key::quit;
                break;
            default:
                break;
            }
            if (out.key != Key::none)
                result.push_back(std::move(out));
        }
        return result;
    }
#endif
    input_event event{};
    for (;;) {
        auto count = read(fd_, &event, sizeof(event));
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
        if (count == 0)
            break;
        if (count != sizeof(event))
            throw std::runtime_error("Keyboard disconnected");
        if (event.type == EV_SYN && event.code == SYN_DROPPED) {
            shift_ = ctrl_ = false;
            input_dropped_ = true;
            *update_shortcut_ = UpdateShortcut{};
            result.push_back({Key::enter_cancel, {}});
            continue;
        }
        if (input_dropped_) {
            if (event.type == EV_SYN && event.code == SYN_REPORT)
                input_dropped_ = false;
            continue;
        }
        if (event.type != EV_KEY)
            continue;
        if (update_shortcut_->event(event.code, event.value)) {
            result.push_back({Key::update, {}, false, true});
            continue;
        }
        if (event.code == KEY_LEFTSHIFT || event.code == KEY_RIGHTSHIFT) {
            shift_ = event.value != 0;
            continue;
        }
        if (event.code == KEY_LEFTCTRL || event.code == KEY_RIGHTCTRL) {
            ctrl_ = event.value != 0;
            continue;
        }
        if (event.code == KEY_CAPSLOCK && event.value == 1) {
            caps_ = !caps_;
            continue;
        }
        if (event.code == KEY_ENTER || event.code == KEY_KPENTER) {
            if (event.value != 2)
                result.push_back(
                    {event.value ? Key::enter_down : Key::enter_up, {}, shift_, ctrl_});
            continue;
        }
        if (!event.value ||
            ((event.code == KEY_SPACE || event.code == KEY_MUTE) && event.value == 2))
            continue;
        auto input = map_key(event.code, shift_, caps_, ctrl_);
        if (input.key != Key::none)
            result.push_back(std::move(input));
    }
    return result;
}
} // namespace okou

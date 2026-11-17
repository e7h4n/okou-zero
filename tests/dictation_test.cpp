// SPDX-License-Identifier: MIT
#include "okou/dictation.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr std::size_t rate = 16000;
void check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
template <typename F> std::string failure(F action) {
    try {
        action();
    } catch (const std::runtime_error &e) {
        return e.what();
    }
    throw std::runtime_error("Expected runtime_error");
}
std::vector<std::int16_t> audio(std::size_t count) {
    std::vector<std::int16_t> pcm(count);
    for (std::size_t i = 0; i < count; ++i)
        pcm[i] = static_cast<std::int16_t>(i % 30000);
    return pcm;
}
void retry_segment() {
    const auto pcm = audio(30 * rate);
    okou::Dictation d("origin-thread", pcm);
    check(d.pending() && d.thread_id() == "origin-thread", "Capture bound to origin");
    int calls = 0, polish_calls = 0;
    auto transcribe = [&](const auto &slice, const auto &previous, double overlap) {
        ++calls;
        if (calls == 1) {
            check(slice == std::vector<std::int16_t>(pcm.begin(), pcm.begin() + 15 * rate),
                  "First window exact");
            check(previous.empty() && overlap == 0, "First context and overlap");
            return std::string("first");
        }
        if (calls == 2 || calls == 3) {
            check(slice ==
                      std::vector<std::int16_t>(pcm.begin() + 13 * rate, pcm.begin() + 28 * rate),
                  "Retry exact failed window");
            check(previous == "first" && overlap == 2, "Retry retains prior context");
            if (calls == 2)
                throw std::runtime_error("secret-token and private provider response");
            return std::string("second");
        }
        check(calls == 4, "Never reupload first successful segment");
        check(slice == std::vector<std::int16_t>(pcm.begin() + 26 * rate, pcm.end()),
              "Final partial exact");
        check(previous == "second" && overlap == 2, "Final context");
        return std::string("third");
    };
    auto polish = [&](const auto &segments) {
        ++polish_calls;
        check(segments == std::vector<std::string>({"first", "second", "third"}),
              "All recognized segments retained");
        return std::string("polished");
    };
    auto error = failure([&] { d.process(transcribe, polish); });
    check(error.find("secret") == std::string::npos, "Provider error sanitized");
    check(calls == 2 && polish_calls == 0, "No automatic retry");
    check(d.pending() && d.partial_text() == "first", "Partial text available after failure");
    check(d.process(transcribe, polish) == "polished", "Explicit retry finishes");
    check(calls == 4 && polish_calls == 1, "Only failed and remaining segments uploaded");
    check(!d.pending() && d.partial_text().empty(), "Success clears recovery");
    check(d.thread_id() == "origin-thread", "Origin remains available for draft insertion");
    check(d.process(transcribe, polish).empty() && calls == 4 && polish_calls == 1,
          "Success cannot replay callbacks");
}
void retry_polish() {
    okou::Dictation d("thread", audio(16 * rate));
    int transcriptions = 0, polishes = 0;
    auto transcribe = [&](const auto &, const auto &, double) {
        return ++transcriptions == 1 ? "one" : "two";
    };
    auto polish = [&](const auto &segments) {
        ++polishes;
        check(segments == std::vector<std::string>({"one", "two"}),
              "Polish receives retained segments");
        if (polishes == 1)
            throw 42; // Even non-standard callback exceptions sanitized.
        return std::string("done");
    };
    failure([&] { d.process(transcribe, polish); });
    check(d.pending() && d.partial_text() == "one two", "Polish failure preserves recognized text");
    check(transcriptions == 2 && polishes == 1, "No automatic polish retry");
    check(d.process(transcribe, polish) == "done", "Polish retry succeeds");
    check(transcriptions == 2 && polishes == 2, "Retry only polishes");
}
void windows_and_bounds() {
    const std::vector<std::size_t> sizes{
        0,         1,         2 * rate,      13 * rate, 15 * rate, 15 * rate + 1,
        26 * rate, 28 * rate, 28 * rate + 1, 600 * rate};
    for (const auto size : sizes) {
        const auto pcm = audio(size);
        okou::Dictation d("thread", pcm);
        std::size_t offset = 0, calls = 0, polishes = 0;
        auto result = d.process(
            [&](const auto &slice, const auto &, double seconds) {
                const auto end = std::min(offset + 15 * rate, size);
                check(slice == std::vector<std::int16_t>(pcm.begin() + offset, pcm.begin() + end),
                      "Exact sample window");
                check(seconds == (offset == 0 ? 0 : 2), "Exact overlap duration");
                check(offset == 0 || slice.size() > 2 * rate, "Never upload overlap-only suffix");
                ++calls;
                offset = end == size ? size : offset + 13 * rate;
                return std::string("x");
            },
            [&](const auto &segments) {
                ++polishes;
                check(segments.size() == calls, "One retained entry per successful window");
                return std::string("ok");
            });
        const auto expected =
            size == 0
                ? 0
                : (size <= 15 * rate ? 1 : 1 + (size - 15 * rate + 13 * rate - 1) / (13 * rate));
        check(calls == expected && polishes == (size ? 1U : 0U), "Boundary callback counts");
        check(result == (size ? "ok" : "") && !d.pending(), "Boundary completion");
    }
    failure([] { okou::Dictation too_long("thread", audio(600 * rate + 1)); });
}
void silence_and_empty_polish() {
    okou::Dictation d("thread", std::vector<std::int16_t>(16 * rate, 0));
    int calls = 0, polishes = 0;
    check(d.process(
               [&](const auto &, const auto &previous, double) {
                   ++calls;
                   check(previous.empty(), "Empty transcript is valid context");
                   return std::string{};
               },
               [&](const auto &segments) {
                   ++polishes;
                   check(segments == std::vector<std::string>({"", ""}),
                         "Empty recognition retained as success");
                   return std::string{};
               })
              .empty(),
          "Empty polished output valid");
    check(calls == 2 && polishes == 1 && !d.pending(), "Silence is not retried");
}
void cancellation() {
    // Before callbacks, after transcription, after polish, and throwing callbacks.
    for (int stage = 0; stage < 5; ++stage) {
        okou::Dictation d("origin", audio(16 * rate));
        bool cancel = stage == 0;
        int calls = 0, polishes = 0;
        std::string inserted;
        auto error = failure([&] {
            inserted = d.process(
                [&](const auto &, const auto &, double) {
                    ++calls;
                    if (stage == 1 || stage == 3)
                        cancel = true;
                    if (stage == 3)
                        throw std::runtime_error("sensitive");
                    return std::string("recognized");
                },
                [&](const auto &) {
                    ++polishes;
                    cancel = true;
                    if (stage == 4)
                        throw std::runtime_error("sensitive");
                    return std::string("must-not-insert");
                },
                [&] { return cancel; });
        });
        check(error == "Dictation cancelled" && inserted.empty(),
              "Cancel prevents final insertion and sanitizes");
        check(!d.pending() && d.partial_text().empty(), "Cancel discards sensitive recovery");
        check(stage != 0 || calls == 0, "Pre-cancel prevents upload");
        check((stage != 1 && stage != 3) || (calls == 1 && polishes == 0),
              "Post-transcribe cancellation prevents polish");
        d.discard();
        d.discard();
        d.process(
            [&](const auto &, const auto &, double) {
                throw std::runtime_error("called");
                return std::string{};
            },
            [&](const auto &) {
                throw std::runtime_error("called");
                return std::string{};
            });
    }
    okou::Dictation d("thread", audio(1));
    check(failure([&] {
              d.process({}, {}, []() -> bool { throw std::runtime_error("secret"); });
          }) == "Dictation cancelled",
          "Cancellation predicate fails closed");
    check(!d.pending(), "Predicate failure clears state");
}
void explicit_discard_and_missing_callbacks() {
    okou::Dictation d("thread", audio(1));
    failure([&] { d.process({}, {}); });
    check(d.pending(), "Missing transcription callback preserves retry");
    int calls = 0;
    auto transcribe = [&](const auto &, const auto &, double) {
        ++calls;
        return std::string("recognized");
    };
    failure([&] { d.process(transcribe, {}); });
    check(calls == 1 && d.partial_text() == "recognized", "Missing polish preserves transcript");
    d.discard();
    check(!d.pending() && d.partial_text().empty(), "Explicit discard clears state");
}
} // namespace

int main() {
    try {
        retry_segment();
        retry_polish();
        windows_and_bounds();
        silence_and_empty_polish();
        cancellation();
        explicit_discard_and_missing_callbacks();
        std::cout << "dictation_test passed (synthetic PCM only; no recording/network)\n";
        return EXIT_SUCCESS;
    } catch (const std::exception &e) {
        std::cerr << "dictation_test failed: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
}

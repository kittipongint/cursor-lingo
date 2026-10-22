// Run: c++ -std=c++17 -I core tests/layoutfix_test.cpp -o /tmp/lf_test && /tmp/lf_test
#include "layoutfix.h"
#include <cstdio>
#include <string>
#include <vector>

using namespace lf;

static std::vector<uint8_t> keysFor(const char16_t* text, Lang lang) {
    std::vector<uint8_t> out;
    for (const char16_t* p = text; *p; p++) {
        int found = -1;
        for (int i = 0; i < kKeyCount * 2 && found < 0; i++) {
            uint8_t k = (uint8_t)((i % kKeyCount) | (i >= kKeyCount ? kShiftBit : 0));
            if (charFor(k, lang) == *p) found = k;
        }
        if (found < 0) { printf("untypable char U+%04X\n", *p); std::exit(2); }
        out.push_back((uint8_t)found);
    }
    return out;
}

static int failures = 0;
static void expect(const char* what, const char16_t* typed, Lang onScreen, bool want, bool finished = true) {
    auto k = keysFor(typed, onScreen);
    bool got = shouldConvert(k.data(), (int)k.size(), onScreen, finished);
    if (got != want) { printf("FAIL: %s (want %d, got %d)\n", what, want, got); failures++; }
}

int main() {
    // Mapping sanity: the same keys read through the other layout.
    {
        auto k = keysFor(u"l;ylfu;yoouhgik", LANG_EN);
        char16_t th[64];
        render(k.data(), (int)k.size(), LANG_TH, th);
        std::u16string s(th);
        if (s != u"สวัสดีวันนี้เรา") { printf("FAIL: mapping\n"); failures++; }
        auto k2 = keysFor(u"ะ้รห", LANG_TH);
        char16_t en[64];
        render(k2.data(), (int)k2.size(), LANG_EN, en);
        if (std::u16string(en) != u"this") { printf("FAIL: mapping 2\n"); failures++; }
    }

    // Thai typed on the English layout -> convert.
    expect("sawasdee", u"l;ylfu;yoouhgik", LANG_EN, true);
    expect("sawasdee (mid-word)", u"l;ylfu;yoouhgik", LANG_EN, true, false);
    expect("khob khun", u"ขอบคุณ", LANG_TH, false);
    {   // Common Thai words typed on the English layout (the Thai text is what the keys mean).
        const char16_t* words[] = {u"ขอบคุณ", u"ตอนนี้", u"ช่วย", u"หน่อย", u"เรียบร้อย", u"มี", u"ทำงาน"};
        for (auto w : words) {
            auto k = keysFor(w, LANG_TH);
            if (!shouldConvert(k.data(), (int)k.size(), LANG_EN, true)) { printf("FAIL: Thai word not fixed\n"); failures++; }
        }
    }
    // English typed on the Thai layout -> convert.
    expect("this on Thai layout", u"ะ้รห", LANG_TH, true);
    expect("hello on Thai layout", u"้ำสสน", LANG_TH, true);

    // Correct text must never change.
    expect("English word", u"hello", LANG_EN, false);
    expect("English word 2", u"keyboard", LANG_EN, false);
    expect("English with comma", u"hello,", LANG_EN, false);
    expect("contraction", u"don't", LANG_EN, false);
    expect("Thai word", u"สวัสดี", LANG_TH, false);
    expect("Thai sentence run", u"ผมกำลังพิมพ์ภาษาไทย", LANG_TH, false);
    expect("short English", u"is", LANG_EN, false);
    expect("short English + comma", u"a,", LANG_EN, false);
    expect("two digits", u"4k", LANG_EN, false);
    expect("cli flag", u"-e", LANG_EN, false);
    expect("number", u"2024", LANG_EN, false);
    expect("path", u"src/main.swift", LANG_EN, false);
    expect("email", u"me@example.com", LANG_EN, false);
    expect("code", u"std::vector", LANG_EN, false);
    expect("mid-word Thai prefix is left alone", u"สวัสดี", LANG_TH, false, false);

    // Selections are converted character by character, keeping what is not on the keyboard.
    {
        const char16_t* in = u"ะ้รห รห ฟ ืำไ ทนกำส\nl;ylfu 🙂";
        int n = (int)std::u16string(in).size();
        std::u16string a(n, 0), b(n, 0);
        int to = convertText(in, n, &a[0]);
        if (to != LANG_EN || a.substr(0, 19) != u"this is a new model") { printf("FAIL: selection TH->EN\n"); failures++; }
        // Mixed text follows the dominant script: here it is now mostly Latin, so it goes back to Thai.
        int back = convertText(a.c_str(), n, &b[0]);
        if (back != LANG_TH || b.substr(0, 19) != std::u16string(in).substr(0, 19)) { printf("FAIL: selection round trip\n"); failures++; }
        char16_t z[4];
        if (convertText(u"123", 3, z) != -1) { printf("FAIL: digits only\n"); failures++; }
    }

    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("all layoutfix tests passed\n");
    return 0;
}

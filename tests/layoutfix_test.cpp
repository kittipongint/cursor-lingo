// Run all tests (from the repo root):
//   c++ -std=c++17 -I core tests/layoutfix_test.cpp -o /tmp/lf_test && /tmp/lf_test
// Ask about one word, as it appears on screen in the given layout:
//   /tmp/lf_test en "l;ylfu"      /tmp/lf_test th "ะ้รห"
// Real-world cases live in tests/cases.tsv; add a line there when you find a wrong or missed fix.
#include "layoutfix.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
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

static std::u16string fromUtf8(const std::string& s) {
    std::u16string out;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = s[i];
        int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
        uint32_t cp = len == 1 ? c : len == 2 ? c & 0x1F : len == 3 ? c & 0x0F : c & 0x07;
        for (int j = 1; j < len && i + j < s.size(); j++) cp = (cp << 6) | (s[i + j] & 0x3F);
        i += len;
        if (cp < 0x10000) out += (char16_t)cp;
        else { cp -= 0x10000; out += (char16_t)(0xD800 + (cp >> 10)); out += (char16_t)(0xDC00 + (cp & 0x3FF)); }
    }
    return out;
}

static std::string toUtf8(const std::u16string& s) {
    std::string out;
    for (char16_t c : s) {
        if (c < 0x80) out += (char)c;
        else if (c < 0x800) { out += (char)(0xC0 | (c >> 6)); out += (char)(0x80 | (c & 0x3F)); }
        else { out += (char)(0xE0 | (c >> 12)); out += (char)(0x80 | ((c >> 6) & 0x3F)); out += (char)(0x80 | (c & 0x3F)); }
    }
    return out;
}

static bool parseLang(const std::string& s, Lang& out) {
    if (s == "en") { out = LANG_EN; return true; }
    if (s == "th") { out = LANG_TH; return true; }
    return false;
}

// `lf_test en "text"`: what CursorLingo would do with this word.
static int explain(const char* layout, const char* text) {
    Lang lang;
    if (!parseLang(layout, lang)) { printf("layout must be en or th\n"); return 2; }
    std::u16string t = fromUtf8(text);
    auto k = keysFor(t.c_str(), lang);
    std::u16string other(k.size() + 1, 0);
    render(k.data(), (int)k.size(), lang == LANG_EN ? LANG_TH : LANG_EN, &other[0]);
    other.resize(k.size());
    bool atSpace = shouldConvert(k.data(), (int)k.size(), lang, true);
    bool midWord = lang == LANG_EN && shouldConvert(k.data(), (int)k.size(), lang, false);
    printf("on screen (%s): %s\nother layout:     %s\nat Space:         %s\n", layout, text, toUtf8(other).c_str(),
           atSpace ? "converts" : "left alone");
    if (lang == LANG_EN) printf("while typing:     %s\n", midWord ? "converts" : "left alone");
    return 0;
}

// tests/cases.tsv: <layout> TAB <text on screen> TAB <fix|keep>
static int runCases(const char* path, int& count) {
    std::ifstream f(path);
    if (!f) { printf("FAIL: cannot open %s\n", path); return 1; }
    int bad = 0, line = 0;
    std::string row;
    while (std::getline(f, row)) {
        line++;
        if (!row.empty() && row.back() == '\r') row.pop_back();
        if (row.empty() || row[0] == '#') continue;
        size_t a = row.find('\t'), b = a == std::string::npos ? a : row.find('\t', a + 1);
        Lang lang;
        std::string want = b == std::string::npos ? "" : row.substr(b + 1);
        if (b == std::string::npos || !parseLang(row.substr(0, a), lang) || (want != "fix" && want != "keep")) {
            printf("FAIL: %s:%d: expected <en|th> TAB <text> TAB <fix|keep>\n", path, line);
            bad++;
            continue;
        }
        std::u16string t = fromUtf8(row.substr(a + 1, b - a - 1));
        auto k = keysFor(t.c_str(), lang);
        bool got = shouldConvert(k.data(), (int)k.size(), lang, true);
        if (got != (want == "fix")) {
            printf("FAIL: %s:%d: %s\n", path, line, row.c_str());
            bad++;
        }
        count++;
    }
    return bad;
}

static int failures = 0;
static void expect(const char* what, const char16_t* typed, Lang onScreen, bool want, bool finished = true) {
    auto k = keysFor(typed, onScreen);
    bool got = shouldConvert(k.data(), (int)k.size(), onScreen, finished);
    if (got != want) { printf("FAIL: %s (want %d, got %d)\n", what, want, got); failures++; }
}

int main(int argc, char** argv) {
    if (argc == 3) return explain(argv[1], argv[2]);
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

    int cases = 0;
    failures += runCases(argc == 2 ? argv[1] : "tests/cases.tsv", cases);

    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("all layoutfix tests passed (%d cases from cases.tsv)\n", cases);
    return 0;
}

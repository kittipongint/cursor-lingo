// Wrong-keyboard-layout detection for English (US QWERTY) <-> Thai (Kedmanee).
//
// Principle: a keystroke is a *physical key*, and each physical key produces one character in
// every layout. So a word typed in the wrong layout is not random text - it is the right text
// seen through the wrong key map. For every word we therefore compute both readings:
//
//     on screen  : what the current layout produced
//     alternative: what the other layout would have produced from the very same keys
//
// and convert only when the on-screen reading looks like gibberish for its language while the
// alternative looks like a real word of the other one:
//
//   * English plausibility: a letter-trigram model (core/en_model.inc, generated from a word list).
//     Real words are cheap; Thai text typed on a Latin layout is full of impossible sequences and
//     punctuation keys (; ' , . [ ]) inside words.
//   * Thai plausibility: orthographic structure. Thai script has strict rules (a word cannot start
//     with a tone mark or a trailing vowel, vowels must sit on consonants, a leading vowel must be
//     followed by a consonant, ...). English typed on the Thai layout breaks them almost at once.
//
// Precision matters more than recall: wrongly "fixing" correct text is far worse than missing a
// typo, so every rule below errs on the side of leaving the text alone.
//
// Header-only, no allocation, no platform dependencies.

#pragma once
#include <cstdint>
#include <cstring>

namespace lf {

enum Lang { LANG_EN = 0, LANG_TH = 1 };

// A key is its index into the physical-key table below, with bit 7 set when Shift (or Caps) is on.
constexpr int kKeyCount = 47;
constexpr uint8_t kShiftBit = 0x80;
constexpr int kMaxWord = 48;

#include "en_model.inc"

// Physical keys in ANSI order: number row, QWERTY row, home row, bottom row.
static const char kEnBase[] = "`1234567890-=qwertyuiop[]\\asdfghjkl;'zxcvbnm,./";
static const char kEnShift[] = "~!@#$%^&*()_+QWERTYUIOP{}|ASDFGHJKL:\"ZXCVBNM<>?";
static const char16_t kThBase[] = u"_ๅ/-ภถุึคตจขชๆไำพะัีรนยบลฃฟหกดเ้่าสวงผปแอิืทมใฝ";
static const char16_t kThShift[] = u"%+๑๒๓๔ู฿๕๖๗๘๙๐\"ฎฑธํ๊ณฯญฐ,ฅฤฆฏโฌ็๋ษศซ.()ฉฮฺ์?ฒฬฦ";

// Platform key codes for the same 47 physical keys, in the same order.
static const uint8_t kMacKeycode[kKeyCount] = {
    50, 18, 19, 20, 21, 23, 22, 26, 28, 25, 29, 27, 24,  // ` 1 2 3 4 5 6 7 8 9 0 - =
    12, 13, 14, 15, 17, 16, 32, 34, 31, 35, 33, 30, 42,  // q w e r t y u i o p [ ] backslash
    0, 1, 2, 3, 5, 4, 38, 40, 37, 41, 39,                // a s d f g h j k l ; '
    6, 7, 8, 9, 11, 45, 46, 43, 47, 44};                 // z x c v b n m , . /
static const uint8_t kWinScan[kKeyCount] = {             // set-1 make codes
    0x29, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x2B,
    0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28,
    0x2C, 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35};

inline int keyIndexFrom(const uint8_t* table, int code) {
    for (int i = 0; i < kKeyCount; i++) if (table[i] == code) return i;
    return -1;
}

inline char16_t charFor(uint8_t key, Lang lang) {
    int i = key & 0x7F;
    if (i >= kKeyCount) return 0;
    bool s = key & kShiftBit;
    if (lang == LANG_EN) return (char16_t)(s ? kEnShift[i] : kEnBase[i]);
    return s ? kThShift[i] : kThBase[i];
}

// Renders `n` keys in `lang`. `out` must hold n+1 code units. Returns n.
inline int render(const uint8_t* keys, int n, Lang lang, char16_t* out) {
    for (int i = 0; i < n; i++) out[i] = charFor(keys[i], lang);
    out[n] = 0;
    return n;
}

// Key that produces `c` in `lang` (with kShiftBit when shifted), or -1.
inline int keyFor(char16_t c, Lang lang) {
    if (c == 0) return -1;
    for (int i = 0; i < kKeyCount; i++) {
        if (charFor((uint8_t)i, lang) == c) return i;
        if (charFor((uint8_t)(i | kShiftBit), lang) == c) return i | kShiftBit;
    }
    return -1;
}

// Re-reads arbitrary text (a selection) through the other layout, character by character: each character is
// mapped back to the key that typed it and forward to that key's character in the other layout. Characters not
// on the keyboard (spaces, new lines, emoji, ...) are kept. The direction follows the script that dominates:
// mostly Thai -> English, otherwise English -> Thai. Writes n units to `out`; returns the target layout, or -1
// when nothing would change.
inline int convertText(const char16_t* in, int n, char16_t* out) {
    int thai = 0, latin = 0;
    for (int i = 0; i < n; i++) {
        if (in[i] >= 0x0E01 && in[i] <= 0x0E5B) thai++;
        else if ((in[i] >= 'a' && in[i] <= 'z') || (in[i] >= 'A' && in[i] <= 'Z')) latin++;
    }
    if (thai == 0 && latin == 0) return -1;
    const Lang from = thai > latin ? LANG_TH : LANG_EN, to = from == LANG_TH ? LANG_EN : LANG_TH;
    bool changed = false;
    for (int i = 0; i < n; i++) {
        int k = keyFor(in[i], from);
        out[i] = k < 0 ? in[i] : charFor((uint8_t)k, to);
        changed |= out[i] != in[i];
    }
    return changed ? (int)to : -1;
}

// ---------------------------------------------------------------------------------------------
// English
// ---------------------------------------------------------------------------------------------

inline bool isTrailingPunct(char16_t c) {
    return c == '.' || c == ',' || c == ';' || c == ':' || c == '!' || c == '?' || c == '\'' || c == '"' || c == ')';
}

// Average cost (nats per letter) of an English-looking reading, or a large value when it contains
// characters that never occur inside an English word. Lower = more English.
// `complete` = false for a word still being typed: its end is not a word boundary yet.
inline float englishCost(const char16_t* s, int n, bool complete = true) {
    while (n > 0 && isTrailingPunct(s[n - 1])) n--;  // "word," "word?" "don'"
    if (n == 0) return 99.0f;
    // Contractions: strip 's / 't / 'd / 'm / 'll / 're / 've (only these: ' is also Thai ง).
    if (n >= 3 && s[n - 2] == '\'') {
        char16_t c = s[n - 1] | 0x20;
        if (c == 's' || c == 't' || c == 'd' || c == 'm') n -= 2;
    } else if (n >= 4 && s[n - 3] == '\'') {
        char16_t c1 = s[n - 2] | 0x20, c2 = s[n - 1] | 0x20;
        if ((c1 == 'l' && c2 == 'l') || (c1 == 'r' && c2 == 'e') || (c1 == 'v' && c2 == 'e')) n -= 3;
    }
    int a = 0, b = 0, cost = 0;
    const int last = complete ? n : n - 1;
    for (int i = 0; i <= last; i++) {
        int c = 0;
        if (i < n) {
            char16_t ch = s[i];
            if (ch >= 'A' && ch <= 'Z') ch = (char16_t)(ch + 32);
            if (ch >= 'a' && ch <= 'z') c = ch - 'a' + 1;
            else cost += 16 * 6;  // digit / symbol inside the word
        }
        cost += kEnCost[(a * 27 + b) * 27 + c];
        a = b;
        b = c;
        if (i < n && c == 0) { a = 0; b = 0; }
    }
    return cost / 16.0f / (float)(last + 1);
}

// ---------------------------------------------------------------------------------------------
// Thai
// ---------------------------------------------------------------------------------------------

inline bool thCons(char16_t c) { return c >= 0x0E01 && c <= 0x0E2E; }
inline bool thLead(char16_t c) { return c >= 0x0E40 && c <= 0x0E44; }               // เ แ โ ใ ไ
inline bool thUpper(char16_t c) { return c == 0x0E31 || (c >= 0x0E34 && c <= 0x0E37); }  // ั ิ ี ึ ื
inline bool thLower(char16_t c) { return c == 0x0E38 || c == 0x0E39; }              // ุ ู
inline bool thTone(char16_t c) { return c >= 0x0E48 && c <= 0x0E4B; }               // ่ ้ ๊ ๋
inline bool thFollow(char16_t c) { return c == 0x0E30 || c == 0x0E32 || c == 0x0E33; }  // ะ า ำ

// Parses Thai orthography into syllables. `bare` counts the syllables that carry no explicit vowel
// mark (legal - คน, ผม - but a string made mostly of them is what English looks like on the Thai
// layout); a lone consonant counts double.
struct ThaiParse { bool ok; int syllables; int bare; };

inline bool thSecondOfCluster(char16_t a, char16_t b) {
    if (b == 0x0E23 || b == 0x0E25 || b == 0x0E27) return a != 0x0E23 && a != 0x0E25 && a != 0x0E27;  // ร ล ว
    if (a == 0x0E2B) return b == 0x0E07 || b == 0x0E0D || b == 0x0E19 || b == 0x0E21 || b == 0x0E22;  // ห + ง ญ น ม ย
    if (a == 0x0E2D) return b == 0x0E22;                                                              // อ + ย
    return false;
}

// Letters that cannot end a syllable (ผ ฝ ห ฮ ฃ ฅ ฉ ฌ อ-as-consonant is handled as a vowel carrier).
inline bool thFinalOk(char16_t c) {
    return thCons(c) && c != 0x0E1C && c != 0x0E1D && c != 0x0E2B && c != 0x0E2E && c != 0x0E03 && c != 0x0E05 &&
           c != 0x0E09 && c != 0x0E0C;
}

// Syllable grammar (what may follow the initial consonant depends on the leading vowel):
//   [lead] C [cluster] [upper|lower|็|รร] [tone] [follow] [final [final]] [์]
inline ThaiParse parseThai(const char16_t* s, int n) {
    constexpr int INF = 1 << 20;
    if (n == 0 || n > kMaxWord) return {false, 0, 0};
    int best[kMaxWord + 1], cnt[kMaxWord + 1];  // best[i]: min bare weight to parse s[0..i)
    for (int i = 0; i <= n; i++) best[i] = INF, cnt[i] = 0;
    best[0] = 0;
    for (int i = 0; i < n; i++) {
        if (best[i] >= INF) continue;
        char16_t c0 = s[i];
        if ((c0 == 0x0E46 || c0 == 0x0E2F) && i > 0) {  // ๆ ฯ follow a syllable
            if (best[i] < best[i + 1]) { best[i + 1] = best[i]; cnt[i + 1] = cnt[i]; }
            continue;
        }
        int p = i;
        char16_t lead = 0;
        if (thLead(s[p])) lead = s[p++];
        if (p >= n || !thCons(s[p])) continue;
        char16_t init = s[p++];
        for (int cl = 0; cl < 2; cl++) {
            int q = p;
            if (cl) {
                if (q >= n || !thSecondOfCluster(init, s[q])) continue;
                q++;
            }
            // Nucleus: a vowel mark above/below, ็, รร, or nothing.
            int nuc = 0;  // 0 none, 1 upper, 2 lower, 3 ็, 4 รร
            int r = q;
            if (r < n && thUpper(s[r])) { nuc = 1; r++; }
            else if (r < n && thLower(s[r])) { nuc = 2; r++; }
            else if (r < n && s[r] == 0x0E47) { nuc = 3; r++; }
            else if (!lead && r + 1 < n && s[r] == 0x0E23 && s[r + 1] == 0x0E23) { nuc = 4; r += 2; }
            bool tone = false;
            if (r < n && thTone(s[r])) { tone = true; r++; }
            int follow = 0;  // 0 none, 1 ะ, 2 า, 3 ำ, 4 าะ
            if (r < n && thFollow(s[r])) {
                follow = s[r] == 0x0E30 ? 1 : (s[r] == 0x0E32 ? 2 : 3);
                r++;
                if (follow == 2 && lead == 0x0E40 && r < n && s[r] == 0x0E30) { follow = 4; r++; }
            }
            // Vowels written with a consonant letter: -อ (ขอ ตอน ร้อย, เ-อ เธอ), เ-ีย / เ-ือ (เรียบ เมื่อ),
            // and -ว- before a final (ช่วย รวม สวย).
            int carrier = 0;
            if (!follow && r < n) {
                if (s[r] == 0x0E2D && !nuc && (!lead || lead == 0x0E40)) { carrier = 1; r++; }
                else if (lead == 0x0E40 && nuc == 1 &&
                         ((s[q] == 0x0E35 && s[r] == 0x0E22) || (s[q] == 0x0E37 && s[r] == 0x0E2D))) { carrier = 1; r++; }
                else if (!lead && !nuc && s[r] == 0x0E27 && r + 1 < n && thFinalOk(s[r + 1])) { carrier = 2; r++; }
            }
            // Which combinations are real Thai?
            if (nuc && follow) continue;                                           // กิา กุะ ...
            if (lead == 0x0E44 || lead == 0x0E43) { if (nuc || follow) continue; }  // ไ ใ stand alone
            else if (lead == 0x0E42) { if (nuc || (follow && follow != 1)) continue; }  // โ-ะ
            else if (lead == 0x0E41) { if ((nuc && nuc != 3) || (follow && follow != 1)) continue; }  // แ-ะ แ-็
            else if (lead == 0x0E40) { if ((nuc == 2) || (follow == 3)) continue; }  // เ-ีย เ-ือ เ-า เ-าะ เ-ิ เ-็ เ-ะ
            else if (nuc == 3) {}                                                   // ก็
            for (int f = 0; f < 3; f++) {
                int e = r;
                if (f >= 1) { if (e >= n || !thFinalOk(s[e]) || follow == 1 || follow == 3 || follow == 4) continue; e++; }  // ะ ำ เ-าะ are open
                if (f == 2) { if (e >= n || !thCons(s[e])) continue; e++; }
                if (e < n && s[e] == 0x0E4C) e++;
                else if (f == 2) continue;
                if (e < n && (thUpper(s[e]) || thLower(s[e]) || thTone(s[e]) || thFollow(s[e]))) continue;
                if (carrier == 2 && f == 0) continue;                            // -ว- needs its final
                bool explicitVowel = lead || nuc || follow || carrier;
                if (!explicitVowel && tone && f == 0) continue;                 // "C+tone" alone is not a syllable
                if (nuc == 1 && s[q] == 0x0E31 && f == 0) {
                    // ั needs a final consonant (ว included): ตัว ตั้ง
                    continue;
                }
                int weight = explicitVowel ? 0 : (f == 0 && !tone ? 2 : 1);
                int bare = best[i] + weight;
                if (bare < best[e]) { best[e] = bare; cnt[e] = cnt[i] + 1; }
            }
        }
    }
    if (best[n] >= INF) return {false, 0, 0};
    return {true, cnt[n], best[n]};
}

// ---------------------------------------------------------------------------------------------
// Decision
// ---------------------------------------------------------------------------------------------

// Keys whose characters in either layout indicate code, URLs, paths, e-mail or numbers rather than
// prose: we never touch such words.
inline bool isLatin(char16_t c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

inline bool looksLikeCode(const uint8_t* keys, int n) {
    int digits = 0, letters = 0;
    char16_t e[kMaxWord + 1];
    render(keys, n, LANG_EN, e);
    for (int i = 0; i < n; i++) {
        if (e[i] >= '0' && e[i] <= '9') digits++;
        if (isLatin(e[i])) letters++;
        switch (e[i]) {
            // Never part of ordinary prose (these keys are rare Thai marks/digits).
            case '\\': case '@': case '#': case '_': case '~': case '|': case '+': case '*': case '<': case '>':
            case '{': case '}': case '%': case '&': case '$':
                if (i != n - 1) return true;
                break;
            case '/':  // a/b, src/main - but a leading '/' is Thai ฝ (ฝน, ฝาก)
                if (i > 0 && i < n - 1) return true;
                break;
            case '.':  // file.ext, example.com: '.' followed by a short lower-case tail
                if (i >= 2 && n - i - 1 >= 2 && n - i - 1 <= 5) {
                    bool tail = true;
                    for (int j = i + 1; j < n; j++) if (!(e[j] >= 'a' && e[j] <= 'z')) tail = false;
                    if (tail) return true;
                }
                break;
        }
    }
    return letters == 0 || digits == n;
}

// A parse is "real looking" when few of its syllables lack an explicit vowel.
inline bool thaiLooksReal(const ThaiParse& t, int bareQuarters) {
    return t.ok && t.bare * 4 <= t.syllables * bareQuarters;
}

struct Tuning {
    float enGibberish = 3.3f;  // English reading costs more than this (nats/letter) => gibberish
    float enGood = 2.45f;      // English alternative costs less than this => real-looking
    int minKeys = 3;           // shorter words are too ambiguous
    int midWordMinKeys = 6;    // minimum length to act before the word is finished
    int bareLenient = 3;       // Thai on screen counts as valid if at most 3/4 of its syllables lack a vowel mark
    float enGoodHard = 3.3f;   // English alternative threshold when the Thai on screen is structurally impossible
};
static const Tuning kTune = {};

// Should the word typed with `keys` in layout `onScreen` be re-typed in the other layout?
// `finished` is false while the user is still typing the word (stricter).
inline bool shouldConvert(const uint8_t* keys, int n, Lang onScreen, bool finished) {
    const Tuning& t = kTune;
    if (n > kMaxWord) return false;
    if (n < t.minKeys) {
        // Two keys are only enough when the Latin reading contains a key that never sits inside an English
        // word and is no digit, flag or path either: , ; ' [ ]  (",u" = มี, "c]" = แล).
        if (n != 2 || onScreen != LANG_EN || !finished) return false;
        char16_t en2[3], th2[3];
        render(keys, n, LANG_EN, en2);
        render(keys, n, LANG_TH, th2);
        int odd = 0;
        for (int i = 0; i < 2; i++) {
            char16_t c = en2[i];
            if (c == ',' || c == ';' || c == '\'' || c == '[' || c == ']') odd++;
            else if (!isLatin(c)) return false;
        }
        if (odd == 0 || (isLatin(en2[0]) && isTrailingPunct(en2[1]))) return false;  // "a," "I'"
        ThaiParse tp = parseThai(th2, n);
        return tp.ok && tp.bare == 0;
    }
    if (!finished && (n < t.midWordMinKeys || onScreen != LANG_EN)) return false;  // English has spaces; only Thai runs on
    if (looksLikeCode(keys, n)) return false;

    char16_t en[kMaxWord + 1], th[kMaxWord + 1];
    render(keys, n, LANG_EN, en);
    render(keys, n, LANG_TH, th);

    // A word still being typed may end in a leading vowel (เ แ โ ใ ไ) that is waiting for its consonant.
    int thn = n;
    if (!finished) while (thn > 0 && thLead(th[thn - 1])) thn--;
    if (thn == 0) return false;

    // Trailing punctuation keys are ambiguous (',' is Thai ม): judge the stem on the Latin side.
    int stem = n;
    while (stem > 0 && isTrailingPunct(en[stem - 1])) stem--;
    if (stem < t.minKeys && stem < n) stem = n;  // nothing left to judge: use the whole word

    if (onScreen == LANG_EN) {
        // English on screen must look like gibberish...
        // The less clearly Thai the alternative is, the more gibberish-like the English must be.
        float ec = englishCost(en, n, finished);
        ThaiParse tp = parseThai(th, thn);
        if (!tp.ok) return false;
        float need;
        if (tp.bare * 4 <= tp.syllables) need = t.enGibberish;
        else if (tp.bare * 2 <= tp.syllables) need = t.enGibberish + 0.8f;
        else if (tp.bare * 4 <= tp.syllables * 3) need = t.enGibberish + 1.6f;
        else return false;
        return ec >= need;
    } else {
        // Thai on screen must be structurally invalid (or all bare syllables)...
        ThaiParse tp = parseThai(th, thn);
        if (thaiLooksReal(tp, t.bareLenient)) return false;
        // ...and the English reading must look like a real word (more leniently when the Thai is impossible).
        if (englishCost(en, n) > (tp.ok ? t.enGood : t.enGoodHard)) return false;
        for (int i = 0; i < stem; i++) {
            char16_t c = en[i];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '\'')) return false;
        }
        return true;
    }
}

}  // namespace lf

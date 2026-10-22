#include "layoutfix_c.h"
#include "layoutfix.h"

extern "C" {

int lf_key_from_mac_keycode(int keycode) { return lf::keyIndexFrom(lf::kMacKeycode, keycode); }

int lf_render(const uint8_t* keys, int n, int lang, uint16_t* out) {
    return lf::render(keys, n, lang ? lf::LANG_TH : lf::LANG_EN, reinterpret_cast<char16_t*>(out));
}

bool lf_should_convert(const uint8_t* keys, int n, int onScreen, bool finished) {
    return lf::shouldConvert(keys, n, onScreen ? lf::LANG_TH : lf::LANG_EN, finished);
}

bool lf_key_is_letter(int index) {
    char16_t c = lf::charFor((uint8_t)index, lf::LANG_EN);
    return c >= 'a' && c <= 'z';
}

}

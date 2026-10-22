// C interface to core/layoutfix.h, for Swift.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Physical key index (0..46) for a macOS virtual key code, or -1 if the key is not a typing key.
int lf_key_from_mac_keycode(int keycode);
// lang: 0 = English (US), 1 = Thai (Kedmanee). Writes n UTF-16 units to out (capacity n + 1).
int lf_render(const uint8_t* keys, int n, int lang, uint16_t* out);
// Should the word typed with `keys` while `onScreen` was active be re-typed in the other layout?
bool lf_should_convert(const uint8_t* keys, int n, int onScreen, bool finished);
// Re-reads a selection through the other layout (n units in, n units out). Returns the target layout
// (0 = English, 1 = Thai), or -1 when nothing would change.
int lf_convert_text(const uint16_t* in, int n, uint16_t* out);
// Key index (with 0x80 for Shift) that types `c` in `lang`, or -1.
int lf_key_for_char(uint16_t c, int lang);
// Index range of the letter keys (for Caps Lock handling): true when the key is a QWERTY letter.
bool lf_key_is_letter(int index);

#ifdef __cplusplus
}
#endif

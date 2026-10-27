# Security and privacy

CursorLingo's optional Auto-fix observes keystrokes, so we treat anything that could expose what people type as a security issue. Examples:

- typed text being written to disk, logs, the clipboard (without being restored) or the network,
- Auto-fix acting inside password fields or secure input,
- another process being able to read CursorLingo's in-memory words,
- a way to make CursorLingo type text the user didn't type.

**Please report these privately** through [GitHub security advisories](https://github.com/kittipongint/cursor-lingo/security/advisories/new), not in a public issue. We'll reply as soon as we can and credit you in the fix unless you'd rather not be named.

## What CursorLingo does with your typing

- Nothing at all unless you turn Auto-fix on. Then:
- macOS uses a listen-only event tap; Windows uses a low-level keyboard hook. Both exist only while Auto-fix is on.
- Only the current word and the last word are kept, in memory. The one thing saved to disk is the personal word list: words the user converted or undid with double Shift, in the app's preferences (macOS) or `%APPDATA%\CursorLingo\CursorLingo.words` (Windows). The menu can forget them. Nothing is sent anywhere.
- *Remember language per app* saves app names (bundle IDs / exe names) with the layout last used in them.
- Password fields (secure input on macOS, classic password boxes on Windows) and terminals are skipped.
- Converting a selection reads it through Accessibility on macOS when the app supports it; otherwise it uses copy/paste and puts your clipboard back.
- The app makes no network connections. The *Report a Wrong Fix or Bug…* menu item only opens the GitHub issue form in your browser.

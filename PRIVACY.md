# CursorLingo Privacy Policy

**English** · [ภาษาไทย](#นโยบายความเป็นส่วนตัวของ-cursorlingo)

*Effective 27 October 2026. Applies to CursorLingo for Windows (Microsoft Store, installer and portable) and macOS.*

**CursorLingo collects no personal data and sends nothing over the network.** It has no accounts, analytics, telemetry, advertising, crash reporting or update checks.

## What the app does on your computer

**Language badge.** To show your input language next to the pointer, CursorLingo reads the current keyboard layout and the pointer position. These are used immediately and never stored.

**Fix Wrong Language (optional, off by default).** When you turn it on (*Double Shift Only* or *Automatic*), CursorLingo watches the keys you press so it can notice a word typed in the wrong layout (English ⇄ Thai) and type it again in the right one.

- Only the word you are typing and the previous word are kept, in memory, and are dropped as soon as you move on, click, switch apps or use a shortcut.
- Nothing you type is written to disk, logged or sent anywhere, with one exception you control: the **personal word list**. When you convert or undo a word with double Shift, that word (as the keys pressed) is remembered on this computer so the same choice is made next time. *Forget learned words* in the menu deletes the list. Uninstalling on Windows (installer or Store) removes it, as does `uninstall.sh` on macOS.
- Password fields and terminal windows are skipped.
- To convert selected text, CursorLingo reads the selection (on macOS through Accessibility when the app supports it, otherwise with a quick copy and paste). Your clipboard is put back afterwards.

**Remember language per app (optional, off by default)** saves the names of apps (for example `code.exe`) with the keyboard layout you last used in them, on this computer only.

## Where data is stored

Everything stays on your device:

- **Windows:** `%APPDATA%\CursorLingo` (`CursorLingo.ini` for settings and per-app layouts, `CursorLingo.words` for the personal word list), or the app's folder in the portable version. The Microsoft Store version keeps the same files in its own app storage, which Windows deletes when you uninstall.
- **macOS:** the app's preferences (`~/Library/Preferences/io.github.cursorlingo.plist`).

## Permissions

- **Windows:** no special permissions. The Microsoft Store version declares *runFullTrust*, which every classic desktop app needs; CursorLingo uses it for its keyboard and mouse hooks.
- **macOS:** *Input Monitoring* and *Accessibility*, requested only when you turn on Fix Wrong Language. The language badge needs neither.

## Links

*Report a wrong fix or bug…* opens CursorLingo's GitHub page in your web browser. Nothing from the app is attached; what you write there is governed by GitHub's privacy policy.

## Children

CursorLingo does not collect data from anyone, including children.

## Changes and contact

Changes to this policy are published in this file, in the project's public history. Questions: open an issue at <https://github.com/kittipongint/cursor-lingo/issues>, or report privacy or security problems privately at <https://github.com/kittipongint/cursor-lingo/security/advisories/new>.

---

# นโยบายความเป็นส่วนตัวของ CursorLingo

[English](#cursorlingo-privacy-policy) · **ภาษาไทย**

*มีผลตั้งแต่ 27 ตุลาคม 2026 ใช้กับ CursorLingo บน Windows (Microsoft Store, ตัวติดตั้ง และแบบพกพา) และ macOS*

**CursorLingo ไม่เก็บข้อมูลส่วนบุคคล และไม่ส่งข้อมูลใดๆ ออกทางเครือข่าย** ไม่มีบัญชีผู้ใช้ ไม่มีการเก็บสถิติ ไม่มีโฆษณา ไม่มีการส่งรายงานแอปค้าง และไม่มีการตรวจหาอัปเดต

## แอปทำอะไรในเครื่องของคุณ

**ป้ายบอกภาษา:** เพื่อแสดงภาษาที่ใช้อยู่ข้างตัวชี้เมาส์ CursorLingo อ่านแป้นพิมพ์ที่ใช้อยู่และตำแหน่งเมาส์ ใช้ทันทีและไม่ได้บันทึกไว้

**แก้คำที่พิมพ์ผิดภาษา (ไม่บังคับ ปิดไว้เป็นค่าเริ่มต้น):** เมื่อเปิดใช้ (*กด Shift 2 ครั้งเพื่อแก้* หรือ *แก้ให้อัตโนมัติ*) CursorLingo จะดูปุ่มที่คุณกด เพื่อจับคำที่พิมพ์ผิดภาษา (อังกฤษ ⇄ ไทย) แล้วพิมพ์ใหม่ให้ถูก

- เก็บไว้แค่คำที่กำลังพิมพ์กับคำก่อนหน้าในหน่วยความจำ และล้างทิ้งทันทีเมื่อพิมพ์ต่อ คลิก สลับแอป หรือกดคีย์ลัด
- สิ่งที่พิมพ์ไม่ถูกบันทึกลงเครื่อง ไม่ถูกเก็บ log และไม่ถูกส่งไปที่ไหน ยกเว้นสิ่งเดียวที่คุณควบคุมเองได้ คือ **รายการคำส่วนตัว** เมื่อคุณสั่งแก้หรือย้อนคำด้วยการกด Shift 2 ครั้ง คำนั้น (ในรูปของปุ่มที่กด) จะถูกจำไว้ในเครื่องนี้ เพื่อให้ครั้งหน้าทำแบบเดิม เมนู *ล้างคำที่จำไว้* จะลบรายการนี้ การถอนแอปบน Windows (ทั้งตัวติดตั้งและ Store) ก็ลบให้ด้วย รวมถึง `uninstall.sh` บน macOS
- ไม่ทำงานในช่องรหัสผ่านและหน้าต่าง Terminal
- เวลาแปลงข้อความที่เลือก CursorLingo จะอ่านข้อความนั้น (บน macOS ผ่าน Accessibility ถ้าแอปรองรับ ถ้าไม่รองรับจะใช้การ copy และ paste) แล้วคืนค่า clipboard เดิมให้

**จำภาษาของแต่ละแอป (ไม่บังคับ ปิดไว้เป็นค่าเริ่มต้น):** บันทึกชื่อแอป (เช่น `code.exe`) คู่กับแป้นพิมพ์ที่ใช้ล่าสุดในแอปนั้น เก็บในเครื่องนี้เท่านั้น

## ข้อมูลเก็บไว้ที่ไหน

ทุกอย่างอยู่ในเครื่องของคุณ:

- **Windows:** `%APPDATA%\CursorLingo` (`CursorLingo.ini` สำหรับค่าตั้งค่าและภาษาของแต่ละแอป, `CursorLingo.words` สำหรับรายการคำส่วนตัว) หรือในโฟลเดอร์ของแอปสำหรับแบบพกพา เวอร์ชัน Microsoft Store เก็บไฟล์ชุดเดียวกันในพื้นที่ของแอปเอง ซึ่ง Windows จะลบให้เมื่อถอนแอป
- **macOS:** ในไฟล์ preferences ของแอป (`~/Library/Preferences/io.github.cursorlingo.plist`)

## สิทธิ์ที่ใช้

- **Windows:** ไม่ต้องใช้สิทธิ์พิเศษ เวอร์ชัน Microsoft Store ประกาศสิทธิ์ *runFullTrust* ซึ่งแอป desktop แบบดั้งเดิมทุกตัวต้องมี CursorLingo ใช้สิทธิ์นี้สำหรับการฟังแป้นพิมพ์และเมาส์
- **macOS:** *Input Monitoring* และ *Accessibility* ขอเฉพาะตอนเปิดใช้การแก้คำที่พิมพ์ผิดภาษา ป้ายบอกภาษาไม่ต้องใช้สิทธิ์ใดเลย

## ลิงก์

เมนู *แจ้งปัญหาหรือคำที่แก้ผิด…* จะเปิดหน้า GitHub ของ CursorLingo ในเว็บเบราว์เซอร์ โดยไม่แนบข้อมูลใดจากแอป สิ่งที่คุณเขียนบน GitHub อยู่ภายใต้นโยบายความเป็นส่วนตัวของ GitHub

## เด็ก

CursorLingo ไม่เก็บข้อมูลจากใครเลย รวมถึงเด็ก

## การเปลี่ยนแปลงและการติดต่อ

การเปลี่ยนแปลงนโยบายนี้จะเผยแพร่ในไฟล์นี้ และดูประวัติได้ใน repo สาธารณะ มีคำถาม เปิด issue ที่ <https://github.com/kittipongint/cursor-lingo/issues> หรือแจ้งปัญหาความเป็นส่วนตัวหรือความปลอดภัยแบบส่วนตัวที่ <https://github.com/kittipongint/cursor-lingo/security/advisories/new>

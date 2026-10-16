@echo off
rem Build with Visual Studio (run from "x64 Native Tools Command Prompt") or any CMake toolchain.
cmake -S "%~dp0" -B "%~dp0..\build\windows" -A x64 || exit /b 1
cmake --build "%~dp0..\build\windows" --config Release || exit /b 1
echo Built: %~dp0..\build\windows\Release\CursorLingo.exe

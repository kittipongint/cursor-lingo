cmake_minimum_required(VERSION 3.16)
project(CursorLingo CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")  # static CRT, no VC++ redist needed

add_executable(CursorLingo WIN32 CursorLingo.cpp CursorLingo.rc)
target_compile_definitions(CursorLingo PRIVATE _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
target_link_libraries(CursorLingo PRIVATE user32 gdi32 shell32 advapi32 shcore wtsapi32 ole32 uuid)

if(MSVC)
  target_compile_options(CursorLingo PRIVATE /O1 /GS- /W4 /permissive- /utf-8)  # sources are UTF-8
  target_link_options(CursorLingo PRIVATE /MANIFEST:NO /OPT:REF /OPT:ICF)
else()
  target_compile_options(CursorLingo PRIVATE -Os -Wall)
  target_link_options(CursorLingo PRIVATE -static -municode -s)
endif()

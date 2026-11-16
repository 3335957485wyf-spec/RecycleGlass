@echo off
rem RecycleGlass-C - build with MSVC (run from a "x64 Native Tools" prompt)
setlocal

if not exist build mkdir build

rc /nologo /I src /I res /fo build\app.res res\app.rc
if errorlevel 1 goto :fail

cl /nologo /O2 /W3 /utf-8 /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 ^
   /Fo:build\ /Fe:build\RecycleGlass.exe ^
   src\main.c src\render.c src\glass.c src\shell.c src\config.c ^
   src\i18n.c src\droptarget.c src\menu.c src\icon.c build\app.res ^
   /link /SUBSYSTEM:WINDOWS ^
   gdiplus.lib shell32.lib ole32.lib oleaut32.lib uuid.lib shlwapi.lib ^
   user32.lib gdi32.lib advapi32.lib
if errorlevel 1 goto :fail

echo.
echo Built build\RecycleGlass.exe
exit /b 0

:fail
echo.
echo Build failed.
exit /b 1

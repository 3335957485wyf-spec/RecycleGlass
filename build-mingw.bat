@echo off
rem RecycleGlass - build with MinGW-w64 (gcc)
rem   build-mingw.bat   -> build\RecycleGlass.exe
setlocal

where gcc >nul 2>nul || (echo gcc not found in PATH & exit /b 1)
if not exist build mkdir build

set CFLAGS=-O2 -std=gnu11 -Wall -Wextra -Wno-unused-parameter -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -finput-charset=UTF-8 -municode

for %%S in (main render glass shell config i18n droptarget menu icon) do (
    echo cc %%S.c
    gcc %CFLAGS% -c src\%%S.c -o build\%%S.o || goto :fail
)

echo rc app.rc
windres -I src -I res res\app.rc -O coff -o build\app.res || goto :fail

echo link
gcc -municode -mwindows -static-libgcc -o build\RecycleGlass.exe ^
    build\main.o build\render.o build\glass.o build\shell.o build\config.o ^
    build\i18n.o build\droptarget.o build\menu.o build\icon.o build\app.res ^
    -lgdiplus -lshell32 -lole32 -loleaut32 -luuid -lshlwapi -lgdi32 -luser32 -ladvapi32 -lcomdlg32 || goto :fail

echo.
echo Built build\RecycleGlass.exe
exit /b 0

:fail
echo.
echo Build failed.
exit /b 1

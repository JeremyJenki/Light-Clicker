@echo off
windres LightClicker.rc -o resources.o
gcc LightClicker.c resources.o -o LightClicker.exe -mwindows -lshell32 -ladvapi32
if %errorlevel%==0 (
    echo.
    echo Build successful! LightClicker.exe is ready.
) else (
    echo.
    echo Build failed.
)
pause

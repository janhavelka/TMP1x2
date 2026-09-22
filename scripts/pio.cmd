@echo off
setlocal

set "PIO_EXE=%USERPROFILE%\.platformio\penv\Scripts\pio.exe"

if not exist "%PIO_EXE%" (
    >&2 echo VS Code-managed PlatformIO was not found at: "%PIO_EXE%". Stop and report the missing installation; do not install another PlatformIO Core.
    exit /b 1
)

"%PIO_EXE%" %*
exit /b %ERRORLEVEL%

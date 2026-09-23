@echo off
rem Register the native messaging host for the bundled browser extension.
rem Usage:  register-nmhost.cmd <EXTENSION_ID>
rem
rem How to get EXTENSION_ID:
rem   1. Open chrome://extensions (or edge://extensions)
rem   2. Turn on "Developer mode"
rem   3. "Load unpacked" -> select the "extension" folder next to this script
rem   4. Copy the ID shown on the extension card and pass it to this script.
setlocal
if "%~1"=="" (
    echo Usage: register-nmhost.cmd ^<EXTENSION_ID^>
    echo.
    echo Get EXTENSION_ID from chrome://extensions or edge://extensions
    echo ^(enable Developer mode, "Load unpacked", pick the extension folder^).
    echo.
    pause
    exit /b 1
)
"%~dp0idm_nmhost.exe" --register-nmhost %~1
echo.
echo Done. Restart the browser to pick up the native messaging host.
pause

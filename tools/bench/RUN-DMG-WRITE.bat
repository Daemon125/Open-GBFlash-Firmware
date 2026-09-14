@echo off
setlocal
cd /d "%~dp0"
echo.
echo   GBFlash benchmark: Game Boy ROM write only
echo.
echo   This ERASES the DMG cartridge you insert. Saves included.
echo.

set "PY="
where py >nul 2>&1 && set "PY=py -3"
if not defined PY where python >nul 2>&1 && set "PY=python"
if not defined PY (
  echo   Python 3 was not found on PATH.
  echo   Install it from https://www.python.org/downloads/ and tick
  echo   "Add python.exe to PATH" in the installer, then run this again.
  echo.
  pause
  exit /b 1
)

%PY% -c "import serial, dateutil, PIL, packaging" 2>nul
if errorlevel 1 (
  echo   Installing FlashGBX's Python dependencies ...
  %PY% -m pip install --quiet pyserial python-dateutil Pillow packaging requests
  if errorlevel 1 (
    echo.
    echo   Could not install them. Run this by hand, then try again:
    echo       %PY% -m pip install pyserial python-dateutil Pillow packaging requests
    echo.
    pause
    exit /b 1
  )
)

%PY% bench_suite.py --writes-only --yes --skip agb,m3d --dmg-flashcart "ChisFlash MBC3 (2 MiB)" %*
if errorlevel 1 (
  echo.
  echo   The run stopped early. The message above says why.
)
echo.
pause
endlocal

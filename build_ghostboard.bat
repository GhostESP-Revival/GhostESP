@echo off
echo Initializing ESP-IDF environment...
call esp-idf-v5.5.1\export.bat
echo.
echo Starting build for GhostBoard...
python build.py
echo.
echo Build complete!
pause

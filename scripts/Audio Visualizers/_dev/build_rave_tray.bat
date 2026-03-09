@echo off
setlocal
title Build GhostESP Rave Tray

set "SCRIPT_DIR=%~dp0"
if "%SCRIPT_DIR:~-1%"=="\" set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"
for %%I in ("%SCRIPT_DIR%") do set "ROOT_DIR=%%~dpI"
if "%ROOT_DIR:~-1%"=="\" set "ROOT_DIR=%ROOT_DIR:~0,-1%"
set "TRAY_DIR=%ROOT_DIR%\rave_tray"
set "OUT_EXE=%ROOT_DIR%\rave_tray.exe"
set "OUT_WORKER=%ROOT_DIR%\rave_worker.exe"
set "WORKER_SCRIPT=%ROOT_DIR%\_internal\Display_Visualizer.py"
set "PYTHON_EXE="
set "PYTHON_ARGS="

where cargo >nul 2>nul
if errorlevel 1 (
    echo Rust toolchain ^(cargo^) not found on PATH.
    echo Install Rust from https://rustup.rs and try again.
    goto :pause_exit
)

if not exist "%TRAY_DIR%\Cargo.toml" (
    echo Missing %TRAY_DIR%\Cargo.toml
    goto :pause_exit
)

if not exist "%WORKER_SCRIPT%" (
    echo Missing %WORKER_SCRIPT%
    goto :pause_exit
)

call :find_python
if errorlevel 1 goto :pause_exit

echo Building bundled worker ^(rave_worker.exe^) ...
call "%PYTHON_EXE%" %PYTHON_ARGS% -m pip install --upgrade pyinstaller >nul
call "%PYTHON_EXE%" %PYTHON_ARGS% -m PyInstaller --noconfirm --clean --onefile --name rave_worker --distpath "%SCRIPT_DIR%" --workpath "%TRAY_DIR%\build_py" --specpath "%TRAY_DIR%\build_py" "%WORKER_SCRIPT%"
if errorlevel 1 (
    echo Python worker build failed.
    goto :pause_exit
)

echo Building rave_tray ^(release^) ...
call cargo build --release --manifest-path "%TRAY_DIR%\Cargo.toml"
if errorlevel 1 (
    echo Build failed.
    goto :pause_exit
)

copy /Y "%TRAY_DIR%\target\release\rave_tray.exe" "%OUT_EXE%" >nul
if errorlevel 1 (
    echo Build succeeded, but copy failed.
    goto :pause_exit
)

if exist "%OUT_WORKER%" del /Q "%OUT_WORKER%" >nul 2>nul

echo.
echo Built: %OUT_EXE%
echo The worker is embedded into this binary.
echo You only need to distribute rave_tray.exe.
echo Launch %OUT_EXE% to run the tray app.
goto :eof

:find_python
where py >nul 2>nul
if not errorlevel 1 (
    set "PYTHON_EXE=py"
    set "PYTHON_ARGS=-3"
    exit /b 0
)

where python >nul 2>nul
if not errorlevel 1 (
    set "PYTHON_EXE=python"
    set "PYTHON_ARGS="
    exit /b 0
)

echo Python 3 was not found on PATH.
echo Install Python to build the bundled worker executable.
exit /b 1

:pause_exit
echo.
pause
exit /b

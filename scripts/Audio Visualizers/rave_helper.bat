@echo off
setlocal enabledelayedexpansion
title GhostESP Rave Helper

set "SCRIPT_DIR=%~dp0"
set "HELPER_SCRIPT=%SCRIPT_DIR%_internal\Display_Visualizer.py"
set "PYTHON_EXE="
set "PYTHON_ARGS="
set "TARGET=%~1"
set "EXTRA_ARGS="
set "AUDIO_ARGS="

call :find_python
if errorlevel 1 goto :pause_exit

if not exist "%HELPER_SCRIPT%" (
    echo Missing helper script: %HELPER_SCRIPT%
    goto :pause_exit
)

if "%~1"=="" (
    echo.
echo GhostESP Rave Helper
echo --------------------
echo 1. USB serial ^(recommended - auto detect + choose baud^)
echo 2. Wi-Fi / UDP
echo 3. List all serial ports
echo 4. List playback output sources
echo 5. List usable GhostESP ports
echo.
set /p MODE=Choose an option [1-5]: 

    if "!MODE!"=="1" goto :serial_prompt
    if "!MODE!"=="2" goto :udp_prompt
    if "!MODE!"=="3" goto :list_ports
    if "!MODE!"=="4" goto :list_sources
    if "!MODE!"=="5" goto :list_usable_ports

    echo Invalid option.
    goto :pause_exit
)

shift
:collect_args
if "%~1"=="" goto :run_udp
set "EXTRA_ARGS=!EXTRA_ARGS! "%~1""
shift
goto :collect_args

:serial_prompt
echo.
set "BAUD_RATE=115200"
set /p BAUD_INPUT=Enter baud rate [115200]: 
if not "%BAUD_INPUT%"=="" set "BAUD_RATE=%BAUD_INPUT%"
call :prompt_audio_source
echo Using baud rate %BAUD_RATE%.
echo Scanning serial ports for GhostESP...
set "EXTRA_ARGS=--pick-port --baud %BAUD_RATE% %AUDIO_ARGS%"
goto :run_helper

:udp_prompt
echo.
set /p TARGET=Enter device IP (leave blank for auto-discovery + broadcast fallback): 
call :prompt_audio_source
goto :run_udp

:list_ports
echo.
call "%PYTHON_EXE%" %PYTHON_ARGS% "%HELPER_SCRIPT%" --list-ports
goto :pause_exit

:list_usable_ports
echo.
set "BAUD_RATE=115200"
set /p BAUD_INPUT=Enter baud rate for probing [115200]: 
if not "%BAUD_INPUT%"=="" set "BAUD_RATE=%BAUD_INPUT%"
call "%PYTHON_EXE%" %PYTHON_ARGS% "%HELPER_SCRIPT%" --list-usable-ports --baud %BAUD_RATE%
goto :pause_exit

:list_sources
echo.
call "%PYTHON_EXE%" %PYTHON_ARGS% "%HELPER_SCRIPT%" --list-sources
goto :pause_exit

:prompt_audio_source
echo.
set "AUDIO_ARGS="
set /p AUDIO_SOURCE=Enter playback source index or press Enter for default ^(use option 4 to list^): 
if not "%AUDIO_SOURCE%"=="" set "AUDIO_ARGS=--source %AUDIO_SOURCE%"
exit /b

:run_udp
if "%TARGET%"=="" (
    set "EXTRA_ARGS=%AUDIO_ARGS% %EXTRA_ARGS%"
) else (
    set "EXTRA_ARGS=%TARGET% %AUDIO_ARGS% %EXTRA_ARGS%"
)

:run_helper
echo.
echo Launching helper...
call "%PYTHON_EXE%" %PYTHON_ARGS% "%HELPER_SCRIPT%" %EXTRA_ARGS%
set "EXIT_CODE=%ERRORLEVEL%"
echo.
if not "%EXIT_CODE%"=="0" (
    echo Helper exited with code %EXIT_CODE%.
) else (
    echo Helper closed.
)
goto :pause_exit

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
echo Install Python and try again.
exit /b 1

:pause_exit
echo.
pause
exit /b

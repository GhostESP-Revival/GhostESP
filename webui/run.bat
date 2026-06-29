@echo off
chcp 65001 >nul
cd /d "%~dp0"

echo ╔══════════════════════════════════════════╗
echo ║   GhostESP WebUI V2 Builder + Host      ║
echo ╚══════════════════════════════════════════╝
echo.

echo [1/2] Building V2 bundle...
call node build.mjs
if errorlevel 1 (
    echo.
    echo  Build failed. Fix errors and try again.
    pause
    exit /b 1
)

echo.
echo [2/2] Starting mock host...
echo.

:: Check if port 8080 is in use
netstat -ano | findstr ":8080" >nul 2>&1
if not errorlevel 1 (
    echo  Port 8080 is already in use.
    choice /C KDN /M "Kill existing process [K], use different port [D], or do nothing [N]"
    if errorlevel 3 (
        echo.
        echo  Exiting without starting host.
        pause
        exit /b 0
    )
    if errorlevel 2 (
        set /p PORT="Enter port number: "
        goto :run_host
    )
    if errorlevel 1 (
        for /f "tokens=5" %%a in ('netstat -ano ^| findstr ":8080"') do (
            echo  Killing PID %%a...
            taskkill /PID %%a /F >nul 2>&1
            timeout /t 1 /nobreak >nul
            goto :run_host
        )
    )
)

:run_host
if "%PORT%"=="" set PORT=8080
echo.
echo  Opening http://localhost:%PORT%
echo  Press Ctrl+C to stop
echo.
set PORT=%PORT%
call node host.mjs

pause

@echo off
python "%~dp0ghostesp_extcap.py" %* 2>nul
if %errorlevel% neq 0 (
    C:\Users\deki\AppData\Local\Programs\Python\Python311\python.exe "%~dp0ghostesp_extcap.py" %*
)

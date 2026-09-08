@echo off
rem Restart loop for win-agent.ps1 (run this on Windows and keep the window open).
rem
rem NOTE: This file is ASCII only on purpose. cmd.exe reads .bat files using the
rem OEM code page (CP932 on Japanese Windows), so UTF-8 Japanese text would be
rem garbled and can break parsing. The explanation in Japanese is in README.md.
rem
rem win-agent.ps1 exits after it updates itself; this loop starts the new one.
rem It also recovers from any crash. Keep this file small; it is never updated.
rem
rem Usage:  win-agent.bat  [args passed to win-agent.ps1]

:loop
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0win-agent.ps1" %*
echo [win-agent.bat] restarting in 3 seconds (press Ctrl+C twice to stop)
timeout /t 3 /nobreak > nul
goto loop

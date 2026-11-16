@echo off
rem Removes every trace of an Aether install on this PC (shell, taskbar, wallpaper, files, settings). Asks before it deletes.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0aether-clean.ps1" %*
pause

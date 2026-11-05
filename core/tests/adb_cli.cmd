@echo off
python -I "%~dp0adb_cli.py" %*
exit /b %ERRORLEVEL%

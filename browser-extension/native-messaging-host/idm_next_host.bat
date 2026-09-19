@echo off
REM IDM Next Native Messaging Host 启动器（Windows）
REM Chrome 会通过此 bat 启动 Python host 脚本

set "SCRIPT_DIR=%~dp0"
set "PYTHON=C:\Users\yyyy\.workbuddy\binaries\python\versions\3.13.12\python.exe"
if not exist "%PYTHON%" set "PYTHON=python"
if not exist "%PYTHON%" set "PYTHON=py"
set "IDM_NEXT_CLI=D:\visual studio\lianxi\MFC\idm-next\build\idm-next.exe"
set "PYTHONIOENCODING=utf-8"

"%PYTHON%" "%SCRIPT_DIR%idm_next_host.py"

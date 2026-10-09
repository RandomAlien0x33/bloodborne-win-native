@echo off
rem Starts the game on Windows through scripts\run_game.py: it builds the port through MSYS2 when
rem needed and starts it. A release package brings its own Python (python\); a source checkout
rem uses MSYS2's, in C:\msys64 (set BB_MSYS2 otherwise); see README.
rem Usage: run.bat [--game-dir DIR] [bb-probe options...]
setlocal
if not defined BB_MSYS2 set "BB_MSYS2=C:\msys64"
set "BB_PYTHON=%~dp0python\python.exe"
if not exist "%BB_PYTHON%" set "BB_PYTHON=%BB_MSYS2%\clang64\bin\python.exe"
if not exist "%BB_PYTHON%" (
    echo Python not found: neither %~dp0python\python.exe nor MSYS2's at %BB_PYTHON%. Install MSYS2 and the packages listed in README.md, or set BB_MSYS2.
    exit /b 1
)
"%BB_PYTHON%" "%~dp0scripts\run_game.py" %*
exit /b %ERRORLEVEL%

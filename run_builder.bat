@echo off
setlocal
set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "PYTHONPATH=%ROOT%\.tools\pydeps;%ROOT%;%PYTHONPATH%"
cd /d "%ROOT%"
where py >nul 2>nul
if %ERRORLEVEL%==0 (
    py -3 "%ROOT%\builder\build_region_gui.py" %*
) else (
    python "%ROOT%\builder\build_region_gui.py" %*
)
exit /b %ERRORLEVEL%

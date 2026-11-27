@echo off
setlocal
set "ROOT=%~dp0"
cd /d "%ROOT%"
if not exist "%ROOT%sim_engine\OpenSky.exe" (
    echo OpenSky.exe has not been built yet.
    pause
    exit /b 127
)
"%ROOT%sim_engine\OpenSky.exe" %*
exit /b %ERRORLEVEL%

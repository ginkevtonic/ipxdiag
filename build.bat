@echo off
REM build.bat -- compiles IPXDIAG.EXE with OpenWatcom.
REM Adjust the owsetenv.bat path below if you installed OpenWatcom
REM somewhere other than C:\WATCOM.

call C:\WATCOM\owsetenv.bat

wcl -0 -bt=dos -ms diag.c ipx.c timer.c -fe=IPXDIAG.EXE

if %ERRORLEVEL% NEQ 0 (
    echo.
    echo *** BUILD FAILED ***
    exit /b %ERRORLEVEL%
) else (
    echo.
    echo *** Build OK: IPXDIAG.EXE ***
)

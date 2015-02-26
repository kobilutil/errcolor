@echo off
setlocal
if "%1" == "" goto usage
if "%2" == "" goto usage

set num=%3
if "%num%" == "" set num=100

:loop
<nul set /p =%1
<nul 1>&2 set /p =%2

set /a num=%num%-1
if "%num%" == "0" goto :eof

goto loop

:usage
echo USAGE: %~n0 ^<stdout text^> ^<stderr text^> [count] >&2


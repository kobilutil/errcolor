@ECHO OFF
:: this script is ONLY meant to be run under an EXISTING CONSOLE.
:: run it at the cmd.exe prompt or call it from your startup.bat for example.
:: double-clicking it from the explorer wont do anything meaningful.

:: if you want to launch a NEW CONSOLE process with the errcolor installed,
:: then run errcolor.exe directly instead.

SETLOCAL
SET SCRIPTDIR=%~dp0
SET APP=errcolor.exe

:: NOTE: under windows xp using a full path to the app didn't work as expected.
:: for an unknown reason, (START /B %APP%) was launching just another instance of cmd.exe instead.
:: a workaround is to use PUSHD/POPD around the START command to temporary change the 
:: current directory to that of the running script.
::SET APP=%SCRIPTDIR%errcolor.exe

:: it is expected that errcolor.exe is present in script's directory
IF NOT EXIST "%SCRIPTDIR%%APP%" (
	ECHO ERROR: "%SCRIPTDIR%%APP%" not found. >&2
	GOTO :EOF
)

:: get a temporary filename into which errcolor.exe will write its server's pipe name
CALL :GETTEMPNAME

:: start the errcolor.exe but dont wait for it to exit.
:: the application will notice that its stdout is redirected to a file, so instead of
:: creating a new console process it will attach itself to the current console and write
:: its named-pipe server name to stdout, which is redirect here to %TMPFILE%.
PUSHD %SCRIPTDIR%
START /B .\%APP% >%TMPFILE%
POPD

:: loop here untill we are able to read the named-pipe server connection point
:SETVAR
SET /P PIPENAME=<%TMPFILE%
IF "%PIPENAME%"=="" GOTO SETVAR

:: temp file no longer needed
DEL %TMPFILE% >NUL

:: base on the "side effect" described here -
:: http://stackoverflow.com/questions/9878007/how-to-permanently-redirect-standard-error-back-to-the-console-again/9880156#9880156
:: http://stackoverflow.com/questions/12273866/is-there-a-way-to-redirect-only-stderr-to-stdout-not-combine-the-two-so-it-can
:: this will permanatly (in the calling shell as well) redirect the stderr to the pipe
2>%PIPENAME% 3>&2 TYPE NUL > NUL

ECHO StdError Colorer Installed 1>&2

:: Done.
GOTO :EOF

:: taken from
:: http://unserializableone.blogspot.co.il/2009/04/create-unique-temp-filename-with-batch.html
:GETTEMPNAME
SET TMPFILE=%TEMP%\mytempfile-%RANDOM%-%TIME:~6,5%.tmp
IF EXIST "%TMPFILE%" GOTO :GETTEMPNAME 

#include "stdafx.h"
#include <shellapi.h>
#include "utils.h"

static const auto OPTION_DEFAULT_COLOR = FOREGROUND_RED | FOREGROUND_INTENSITY;
static const auto OPTION_DEFAULT_CMDLINE = L"%COMSPEC%"; 
static const auto READ_BUFFER_SIZE = 5 * 1024;

#ifdef _DEBUG
static const auto WAIT_FOR_CONNECTION_TIMEOUT = INFINITE;
#else
static const auto WAIT_FOR_CONNECTION_TIMEOUT = 5000;
#endif

struct Options
{
	WORD	color;
	WCHAR	cmdLine[MAX_PATH];
};

DWORD RunProcess(LPCWSTR cmdLine, LPCWSTR pipeName)
{
	// open up the "other" side (client side) of the pipe. it will be passed directly to the
	// target process as its stdderr.
	SECURITY_ATTRIBUTES sa{};
	sa.nLength = sizeof(sa);
	sa.bInheritHandle = TRUE;
	auto hWritePipe = ::CreateFile(pipeName, GENERIC_WRITE, 0, &sa, OPEN_EXISTING, 0, NULL);
	if (!hWritePipe || (hWritePipe == INVALID_HANDLE_VALUE))
	{
		debug_print("CreateFile(..pipe..) failed. err=%#x\n", GetLastError());
		return false;
	}

	PROCESS_INFORMATION pi{};
	STARTUPINFO si{};
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESTDHANDLES;
	// connect the stderr of the target process to our pipe
	si.hStdError = hWritePipe;

	// HACK: after changing the project from console to windows subsystem, GetStdHandle returns NULL,
	// but since it seems that the initial values of std handles for a newly created console process
	// are always the same - stdin=3, stdout=7 and stderr=11 - we hardcode those here.
	// TODO: find a better solution.
	si.hStdInput = (HANDLE)3;
	si.hStdOutput = (HANDLE)7;
	//si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
	//si.hStdOutput = ::GetStdHandle(STD_OUTPUT_HANDLE);

	// CreateProcess requires a modifiable commandline parameter
	WCHAR cmdLine2[MAX_PATH];
	::ExpandEnvironmentStrings(cmdLine, cmdLine2, ARRAYSIZE(cmdLine2));

	{
		// launching the target process while the Wow64 filesystem redirection is disabled will cause a 64bit Windows 
		// to always choose the 64bit cmd.exe from C:\Windows\system32 instead of the 32bit one from C:\Windows\SysWOW64
		ScopedDisableWow64FsRedirection a;

		if (!::CreateProcess(NULL, cmdLine2, NULL, NULL, TRUE, CREATE_NEW_CONSOLE, NULL, NULL, &si, &pi))
		{
			debug_print("CreateProcess failed. err=%#x, cmdline=%S\n", ::GetLastError(), cmdLine);
			return 0;
		}
	}

	::CloseHandle(pi.hProcess);
	::CloseHandle(pi.hThread);

	// close our copy of the write end of the pipe, so that when the target process has
	// terminated, the code in ReadPipeLoop will receive a broken-pipe error and will exit.
	::CloseHandle(hWritePipe);

	return pi.dwProcessId;
}

bool AttachToConsole(DWORD pid)
{
	// HACK: on windows xp, the first call to AttachConsole was always failing with error 0x6 (The 
	// handle is invalid). subsequent calls were failing too with error 0x5 (Access is denied). 
	// sleeping inside a loop after the failed attempt (even upto 5000ms) didn't help.
	//
	// on windows 7 the first call was failing as well but with error 0x1f (A device attached to 
	// the system is not functioning). yet after a short sleep (even as low as 10ms) the second 
	// call usually succeeded.
	//
	// BUT, when the sleep was called BEFORE calling AttachConsole, on windows xp the call succeeded 
	// in the first attempt!
	// on windows 7 the success was in the first try as well.

	::Sleep(100);

	if (!::AttachConsole(pid))
	{
		debug_print("AttachConsole failed. err=%#x\n", ::GetLastError());
		return false;
	}

	InstallDefaultCtrlHandler();
	return true;
}

void ReadPipeLoop(HANDLE hReadPipe, WORD colorStderr)
{
	// (see HACK in RunProcess for more details)
	auto hStdOut = ::CreateFile(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
	//auto hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);

	// no need for std::vector. 
	// use a constant size buffer instead and save some executable size.
	BYTE buffer[READ_BUFFER_SIZE];

	while (true)
	{
		DWORD num;
		DWORD bytesEvailable = 0;

		// get the number of bytes available in the pipe.
		// also get a peek to the first byte to save an additional call later on (see logic below)
		if (!::PeekNamedPipe(hReadPipe, buffer, 1, &num, &bytesEvailable, nullptr))
		{
			debug_print("PeekNamedPipe failed. err=%#x\n", ::GetLastError());
			break;
		}

		// if no data is available in the pipe then sleep for a short while and try again
		// TODO: find a better solution
		// maybe to issue a blocking read for one byte and then use peek to see if there is more?
		if (!num)
		{
			Sleep(100);
			continue;
		}

		// get the current printing color of the console screen.
		// we use it to restore the printing color after we done printing all the bytes
		// currently present in the pipe.
		CONSOLE_SCREEN_BUFFER_INFO csbi{};
		if (!::GetConsoleScreenBufferInfo(hStdOut, &csbi))
		{
			debug_print("GetConsoleScreenBufferInfo failed. err=%#x\n", ::GetLastError());
			break;
		}

		// change the printing color to something eye catching
		// TODO: maybe to get this as a setting from the user?
		::SetConsoleTextAttribute(hStdOut, colorStderr);

		while (bytesEvailable > 1)
		{
			DWORD bytesToRead = bytesEvailable - 1;
			if (bytesToRead > sizeof(buffer))
				bytesToRead = sizeof(buffer);

			::ReadFile(hReadPipe, buffer, bytesToRead, &num, NULL);
			::WriteFile(hStdOut, buffer, bytesToRead, &num, NULL);

			::PeekNamedPipe(hReadPipe, buffer, 1, &num, &bytesEvailable, NULL);
		}

		// at this point there is still 1 byte in the pipe and we got a peek on it
		// and stored it in the first position in our buffer.

		// write the last bytes
		::WriteFile(hStdOut, buffer, 1, &num, NULL);

		// restore the console's previous printing color
		::SetConsoleTextAttribute(hStdOut, csbi.wAttributes);

		// remove the last byte from the pipe thus allowing the process that is waiting 
		// on the other side of the pipe to continue.
		::ReadFile(hReadPipe, buffer, 1, &num, NULL);
	}
}

void StopFeedbackCursor()
{
	// http://stackoverflow.com/questions/3857054/turning-off-process-feedback-cursor-in-windows
	// posting a dummy message and reading it back seems to trick the to sto displaying the feedback cursor.
	MSG msg;
	::PostMessage(NULL, WM_NULL, 0, 0);
	::GetMessage(&msg, NULL, 0, 0);
}

bool ParseCmdline(Options& opts, int argc, WCHAR* argv[])
{
	opts.color = OPTION_DEFAULT_COLOR;
	::lstrcpy(opts.cmdLine, OPTION_DEFAULT_CMDLINE);

	for (auto i = 1; i < argc; ++i)
	{
		if (::lstrcmp(L"-c", argv[i]) == 0)
		{
			if (++i >= argc)
				return false;

			errno = 0;
			auto c = ::wcstoul(argv[i], NULL, 0);
			if (errno)
				return false;

			if (c < 0 || c >= 256)
				return false;

			opts.color = (WORD)c;
		}
		else
		if (::lstrcmp(L"-e", argv[i]) == 0)
		{
			if (++i >= argc)
				return false;

			opts.cmdLine[0] = 0;
			for (; i < argc; ++i)
			{
				::lstrcat(opts.cmdLine, argv[i]);
				if ((i + 1) < argc)
					::lstrcat(opts.cmdLine, L" ");
			}
		}
		else
			return false;
	}

	return true;
}

HANDLE CreatePipe(LPWSTR pipeName)
{
	// create a unique pipe name
	::wsprintf(pipeName, L"\\\\.\\pipe\\errcolor-%lu-52DDFBE7-CF76-441F-90CF-B7D825D4DAC2", ::GetCurrentProcessId());

	auto hReadPipe = ::CreateNamedPipe(pipeName,
		PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
		PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
		1, // single connection only
		1, // 1 byte buffer
		1, // 1 byte buffer
		NMPWAIT_USE_DEFAULT_WAIT,
		NULL);

	if (!hReadPipe)
	{
		debug_print("CreateNamedPipe failed. err=%#x\n", GetLastError());
		return NULL;
	}

	return hReadPipe;
}

bool WaitForConnection(HANDLE hReadPipe)
{
	OVERLAPPED ov{};
	ov.hEvent = ::CreateEvent(nullptr, TRUE, FALSE, nullptr);
	if (!ov.hEvent)
	{
		debug_print("CreateEvent failed. err=%#x\n", GetLastError());
		return false;
	}

	// check if the pipe if connected
	::ConnectNamedPipe(hReadPipe, &ov);
	auto rc = ::GetLastError();
	switch (rc)
	{
		// client already connected to the pipe
		case ERROR_PIPE_CONNECTED:
			return true;
		// pipe not connected yet. wait a while for the client to connect.
		// return true if connection established, false if timeout occured.
		case ERROR_IO_PENDING:
			return (::WaitForSingleObject(ov.hEvent, WAIT_FOR_CONNECTION_TIMEOUT) == WAIT_OBJECT_0);
		default:
			return false;
	}
}

// Main entry point for a console application
int _tmain(int argc, _TCHAR* argv[])
{
	// since there is no message loop here like a regular windows process have, 
	// the OS will keep displaying its feedback cursor for some time.
	// explicitly stop the feedback cursor.
	StopFeedbackCursor();

	Options opts;
	if (!ParseCmdline(opts, argc, argv))
	{
		debug_print("ParseCmdline failed. argc=%d, cmdline=%S\n", argc, ::GetCommandLine());
		::MessageBox(NULL, L"Usage: errcolor.exe [-c <color>] [-e <cmdline>]", L"Stderr Colorer", MB_OK);
		return 1;
	}

	// create a named pipe
	WCHAR pipeName[MAX_PATH];
	HANDLE hReadPipe = CreatePipe(pipeName);
	if (!hReadPipe)
	{
		debug_print("CreatePipe failed. err=%#x\n", ::GetLastError());
		return 1;
	}

	debug_print("pipe created %S\n", pipeName);

	auto hStdout = ::GetStdHandle(STD_OUTPUT_HANDLE);
	auto stdoutType = ::GetFileType(hStdout);

	// if the stdout is redirected, it means the client is requesting us to 
	// attach to its current console instead of launching a new one.
	// (see "attach-errcolor-to-current-console.bat" for an example of that).
	if ((stdoutType == FILE_TYPE_PIPE) || (stdoutType == FILE_TYPE_DISK))
	{
		debug_print("attaching to current console\n");

		// attach to the console that we are launching from
		if (!AttachToConsole(ATTACH_PARENT_PROCESS))
			return 1;

		// write the name of our named-pipe server back to the client
		file_print(hStdout, "%S\n", pipeName);
		::CloseHandle(hStdout);
	}
	else
	{
		debug_print("launching process - %S\n", opts.cmdLine);

		// launch a new process with its stderr already redirected to our pipe
		auto pid = RunProcess(opts.cmdLine, pipeName);
		if (!pid)
			return 1;

		debug_print("attaching to process's console - pid=%lu\n", pid);

		// and attach to its console
		if (!AttachToConsole(pid))
			return 1;
	}

	debug_print("wait for connection\n");

	// ensure the pipe is connected. 
	// when launching a new process, the connection is already established since we
	// open both side of the pipe and pass the "client" side handle to the new process.
	// but when we are attaching to an existing console, we only open the "server" side 
	// of the pipe and then write the name of our named-pipe back to the client. afterwards 
	// we wait for the client to connect. 
	if (!WaitForConnection(hReadPipe))
	{
		debug_print("WaitForConnection failed. err=%#x\n", ::GetLastError());
		return 1;
	}

	debug_print("connection established\n");

	// keep on reading from our side of the pipe until the other side closes
	// its side of the pipe.
	ReadPipeLoop(hReadPipe, opts.color);

	::CloseHandle(hReadPipe);
	return 0;
}

// Main entry point for a windows application
int APIENTRY _tWinMain(_In_ HINSTANCE hInstance,
	_In_opt_ HINSTANCE hPrevInstance,
	_In_ LPTSTR    lpCmdLine,
	_In_ int       nCmdShow)
{
	int argc;
	auto argv = ::CommandLineToArgvW(::GetCommandLine(), &argc);
	return _tmain(argc, argv);
}

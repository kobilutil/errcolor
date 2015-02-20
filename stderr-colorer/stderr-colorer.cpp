// ConsoleApplication4.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"
#include <stdio.h>
#include <io.h>
#include <vector>

void debug_print(char const* format, ...)
{
#ifdef _DEBUG
	va_list args;
	va_start(args, format);
	char line[200];
	vsprintf(line, format, args);
	va_end(args);
	::OutputDebugStringA(line);
#endif
}

#ifdef _DEBUG
static const auto WAIT_FOR_CONNECTION_TIMEOUT = INFINITE;
#else
static const auto WAIT_FOR_CONNECTION_TIMEOUT = 5000;
#endif

// TODO: get this from the user
//static const auto STDERR_COLOR = FOREGROUND_RED;
static const auto STDERR_COLOR = FOREGROUND_RED | FOREGROUND_INTENSITY;


class App
{
	// non copyable
	App(App const&) = delete;
	App& operator=(App const&) = delete;

public:
	App();
	~App();

	bool Init();
	bool WaitForConnection();
	void Disconnect();
	void PipeReadLoop();

private:
	HANDLE		_hPipe;
	OVERLAPPED	_ov;
};

App::App()
	: _hPipe{}, _ov{}
{}

App::~App()
{
	if (!_hPipe)
		CloseHandle(_hPipe);

	if (_ov.hEvent)
		CloseHandle(_ov.hEvent);
}

bool App::Init()
{
	// ignore ctrl-c and ctrl-break
	::SetConsoleCtrlHandler(
		[](DWORD CtrlType)
		{
#ifndef _DEBUG	// for release version
			switch (CtrlType)
			{
			// ignore ctrl-c and ctrl-break signals
			case CTRL_C_EVENT:
			case CTRL_BREAK_EVENT:
				return TRUE;
			}
#endif
			// for all other signals allow the default processing.
			// whithout this windows xp will display an ugly end-process dialog box
			// when the user tries to close the console window.
			return FALSE;
		},
		TRUE
	);

	_ov.hEvent = ::CreateEvent(nullptr, TRUE, FALSE, nullptr);
	if (!_ov.hEvent)
	{
		debug_print("CreateEvent failed. err%#x\n", GetLastError());
		return false;
	}

	// create a unique pipe name
	WCHAR pipeName[MAX_PATH];
	::wsprintf(pipeName, L"\\\\.\\pipe\\stderr-colorer-52DDFBE7-CF76-441F-90CF-B7D825D4DAC2-%u-%u",
		::GetConsoleWindow(), ::GetTickCount());

	_hPipe = ::CreateNamedPipe(pipeName,
		PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
		PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
		1,	// single connection only
		1,	// 1 byte buffer
		1,	// 1 byte buffer
		NMPWAIT_USE_DEFAULT_WAIT,
		NULL);

	// TODO: maybe to loop untill we succeed?
	if (!_hPipe)
	{
		debug_print("CreateNamedPipe failed. err%#x\n", GetLastError());
		return false;
	}

	debug_print("server created on %S\n", pipeName);

	SECURITY_ATTRIBUTES sa{};
	sa.nLength = sizeof(sa);
	sa.bInheritHandle = TRUE;
	auto hPipeClient = ::CreateFile(pipeName, GENERIC_WRITE, 0, &sa, OPEN_EXISTING, 0, NULL);
	if (!hPipeClient || (hPipeClient == INVALID_HANDLE_VALUE))
	{
		debug_print("CreateFile(..pipe..) failed. err%#x\n", GetLastError());
		return false;
	}

	PROCESS_INFORMATION pi{};
	STARTUPINFO si{};
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
	si.hStdOutput = ::GetStdHandle(STD_OUTPUT_HANDLE);
	si.hStdError = hPipeClient;

	WCHAR cmdLine[MAX_PATH];
	wsprintf(cmdLine, L"cmd.exe /k echo Stderr Colorer Installed 1>&2");

	if (!::CreateProcess(NULL, cmdLine, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi))
	{
		::CloseHandle(hPipeClient);
		debug_print("CreateProcess failed. err%#x\n", GetLastError());
		return false;
	}

	::CloseHandle(hPipeClient);
	::CloseHandle(pi.hProcess);
	::CloseHandle(pi.hThread);

	return true;
}

bool App::WaitForConnection()
{
	if (!::ResetEvent(_ov.hEvent))
	{
		debug_print("ResetEvent failed. err%#x\n", GetLastError());
		return false;
	}

	::ConnectNamedPipe(_hPipe, &_ov);

	auto rc = ::GetLastError();
	switch (rc)
	{
	case ERROR_PIPE_CONNECTED:
		return true;
	case ERROR_IO_PENDING:
		return (WaitForSingleObject(_ov.hEvent, WAIT_FOR_CONNECTION_TIMEOUT) == WAIT_OBJECT_0);
	default:
		return false;
	}
}

void App::Disconnect()
{
	::DisconnectNamedPipe(_hPipe);
}

void App::PipeReadLoop()
{
	auto hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);

	std::vector<BYTE> buffer;
	buffer.resize(1);

	while (true)
	{
		DWORD num;
		DWORD bytesEvailable = 0;

		// get the number of bytes available in the pipe.
		// also get a peek to the first byte to save an additional call later on (see logic below)
		if (!::PeekNamedPipe(_hPipe, buffer.data(), 1, &num, &bytesEvailable, nullptr))
		{
			debug_print("PeekNamedPipe failed. err%#x\n", GetLastError());
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
			debug_print("GetConsoleScreenBufferInfo failed. err%#x\n", GetLastError());
			break;
		}

		// change the printing color to something eye catching
		// TODO: maybe to get this as a setting from the user?
		::SetConsoleTextAttribute(hStdOut, STDERR_COLOR);

		while (bytesEvailable > 1)
		{
			buffer.resize(bytesEvailable);

			::ReadFile(_hPipe, buffer.data(), bytesEvailable - 1, &num, NULL);
			::WriteFile(hStdOut, buffer.data(), bytesEvailable - 1, &num, NULL);

			::PeekNamedPipe(_hPipe, buffer.data(), 1, &num, &bytesEvailable, NULL);
		}

		// at this point there is still 1 byte in the pipe and we got a peek on it
		// and stored it in the first position in our buffer.

		// write the last bytes
		::WriteFile(hStdOut, buffer.data(), 1, &num, NULL);

		// restore the console's previous printing color
		::SetConsoleTextAttribute(hStdOut, csbi.wAttributes);

		// remove the last byte from the pipe thus allowing the process that is waiting 
		// on the other side of the pipe to continue.
		::ReadFile(_hPipe, buffer.data(), 1, &num, NULL);
	}
}

int _tmain(int argc, _TCHAR* argv[])
{
	App app;

	if (!app.Init())
		return 1;

	if (!app.WaitForConnection())
		return 1;

	app.PipeReadLoop();

	app.Disconnect();

	return 0;
}

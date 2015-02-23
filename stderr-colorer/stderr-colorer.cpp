// ConsoleApplication4.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"
#include <stdio.h>
#include <vector>
//#include <Shlwapi.h>
#include <shellapi.h>

// TODO: get this from the user
//static const auto STDERR_COLOR = FOREGROUND_RED;
static const auto STDERR_COLOR = FOREGROUND_RED | FOREGROUND_INTENSITY;
static const auto DEFAULT_CHILD_PROCESS_CMDLINE = L"cmd.exe /k echo Stderr Colorer Installed 1>&2";

//
// a helper macro that uses c++ compile-time type detection to ease the usage of the
// GetProcAddress api. For example, compare
//
// this:
//		typedef BOOL(WINAPI *LPFN_Wow64DisableWow64FsRedirection) (PVOID *OldValue);
//		static auto func = (LPFN_Wow64DisableWow64FsRedirection)::GetProcAddress(::GetModuleHandle(L"kernel32"), "Wow64DisableWow64FsRedirection");
//
// to that:
//		static auto func = GETPROC(Wow64DisableWow64FsRedirection, kernel32);
//
#define GETPROC(F,M) (decltype(&F))::GetProcAddress(::GetModuleHandle(L#M), #F)

// a simple printf style wrapper for OutputDebugString
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

// temporary disable the Wow64 file system redirection for 32bit process under 64bit OS
class ScopedDisableWow64FsRedirection
{
	// noncopyable
	ScopedDisableWow64FsRedirection(ScopedDisableWow64FsRedirection const&) = delete;
	ScopedDisableWow64FsRedirection& operator=(ScopedDisableWow64FsRedirection const&) = delete;

public:
	ScopedDisableWow64FsRedirection()
	{
		if (IsWow64())
			_redirected = DisableWow64FsRedirection(&_ctx);
	}
	~ScopedDisableWow64FsRedirection()
	{
		if (_redirected)
			RevertWow64FsRedirection(_ctx);
	}

private:
	bool	_redirected{};
	PVOID	_ctx{};

private:
	static bool IsWow64()
	{
		static auto func = GETPROC(IsWow64Process, kernel32);
		if (!func)
			return false;
	
		BOOL bIsWow64;
		if (!func(::GetCurrentProcess(), &bIsWow64))
			return false;

		return bIsWow64 == TRUE;
	}

	static bool DisableWow64FsRedirection(PVOID* ctx)
	{
		static auto func = GETPROC(Wow64DisableWow64FsRedirection, kernel32);
		if (!func)
			return false;
	
		return func(ctx) == TRUE;
	}

	static bool RevertWow64FsRedirection(PVOID ctx)
	{
		static auto func = GETPROC(Wow64RevertWow64FsRedirection, kernel32);
		if (!func)
			return false;

		return func(ctx) == TRUE;
	}
};


bool RunProcess(LPCWSTR cmdLine, HANDLE hWritePipe)
{
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
	::wsprintf(cmdLine2, cmdLine);

	{
		// launching the target process while the Wow64 filesystem redirection is disabled will cause a 64bit Windows 
		// to always choose the 64bit cmd.exe from C:\Windows\system32 instead of the 32bit one from C:\Windows\SysWOW64
		ScopedDisableWow64FsRedirection a;

		if (!::CreateProcess(NULL, cmdLine2, NULL, NULL, TRUE, CREATE_NEW_CONSOLE, NULL, NULL, &si, &pi))
		{
			debug_print("CreateProcess failed. err%#x, cmdline=%S\n", ::GetLastError(), cmdLine);
			return false;
		}
	}

	::CloseHandle(pi.hProcess);
	::CloseHandle(pi.hThread);

	for (auto i = 0; i < 5; ++i)
	{
		if (::AttachConsole(pi.dwProcessId))
		{
			debug_print("AttachConsole OK. pid=%lu, i=%d\n", pi.dwProcessId, i);
			return true;
		}

		::Sleep(500);
	}

	debug_print("AttachConsole failed. err%#x\n", ::GetLastError());
	return false;
}

void ReadPipeLoop(HANDLE hReadPipe)
{
	// (see HACK in RunProcess for more details)
	auto hStdOut = ::CreateFile(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
	//auto hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);

	std::vector<BYTE> buffer;
	buffer.resize(1);

	while (true)
	{
		DWORD num;
		DWORD bytesEvailable = 0;

		// get the number of bytes available in the pipe.
		// also get a peek to the first byte to save an additional call later on (see logic below)
		if (!::PeekNamedPipe(hReadPipe, buffer.data(), 1, &num, &bytesEvailable, nullptr))
		{
			debug_print("PeekNamedPipe failed. err%#x\n", ::GetLastError());
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
			debug_print("GetConsoleScreenBufferInfo failed. err%#x\n", ::GetLastError());
			break;
		}

		// change the printing color to something eye catching
		// TODO: maybe to get this as a setting from the user?
		::SetConsoleTextAttribute(hStdOut, STDERR_COLOR);

		while (bytesEvailable > 1)
		{
			buffer.resize(bytesEvailable);

			::ReadFile(hReadPipe, buffer.data(), bytesEvailable - 1, &num, NULL);
			::WriteFile(hStdOut, buffer.data(), bytesEvailable - 1, &num, NULL);

			::PeekNamedPipe(hReadPipe, buffer.data(), 1, &num, &bytesEvailable, NULL);
		}

		// at this point there is still 1 byte in the pipe and we got a peek on it
		// and stored it in the first position in our buffer.

		// write the last bytes
		::WriteFile(hStdOut, buffer.data(), 1, &num, NULL);

		// restore the console's previous printing color
		::SetConsoleTextAttribute(hStdOut, csbi.wAttributes);

		// remove the last byte from the pipe thus allowing the process that is waiting 
		// on the other side of the pipe to continue.
		::ReadFile(hReadPipe, buffer.data(), 1, &num, NULL);
	}
}

// Main entry point for a console application
int _tmain(int argc, _TCHAR* argv[])
{
	::SetConsoleCtrlHandler(
		[](DWORD CtrlType)
		{
#ifndef _DEBUG // for release version only
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

	HANDLE hReadPipe{}, hWritePipe{};
	if (!::CreatePipe(&hReadPipe, &hWritePipe, NULL, 1))
	{
		debug_print("CreatePipe failed. err%#x\n", ::GetLastError());
		goto error;
	}

	if (!::SetHandleInformation(hWritePipe, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT))
	{
		debug_print("SetHandleInformation failed. err%#x\n", ::GetLastError());
		goto error;
	}

	if (RunProcess(DEFAULT_CHILD_PROCESS_CMDLINE, hWritePipe))
		ReadPipeLoop(hReadPipe);

	return 0;

error:
	::CloseHandle(hReadPipe);
	::CloseHandle(hWritePipe);
	return 1;
}

// Main entry point for a windows application
int APIENTRY _tWinMain(_In_ HINSTANCE hInstance,
	_In_opt_ HINSTANCE hPrevInstance,
	_In_ LPTSTR    lpCmdLine,
	_In_ int       nCmdShow)
{
	int argc;
	auto argv = ::CommandLineToArgvW(lpCmdLine, &argc);
	return _tmain(argc, argv);
}

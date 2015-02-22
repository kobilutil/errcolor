// ConsoleApplication4.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"
#include <stdio.h>
#include <vector>

// TODO: get this from the user
//static const auto STDERR_COLOR = FOREGROUND_RED;
static const auto STDERR_COLOR = FOREGROUND_RED | FOREGROUND_INTENSITY;
static const auto DEFAULT_CHILD_PROCESS_CMDLINE = L"cmd.exe /k echo Stderr Colorer Installed 1>&2";

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

class ScopedDisableWow64FsRedirection
{
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
		typedef BOOL(WINAPI *LPFN_ISWOW64PROCESS) (HANDLE, PBOOL);
		static auto func = (LPFN_ISWOW64PROCESS)::GetProcAddress(::GetModuleHandle(L"kernel32"), "IsWow64Process");
	
		if (!func)
			return false;
	
		BOOL bIsWow64;
		if (!func(::GetCurrentProcess(), &bIsWow64))
			return false;

		return bIsWow64 == TRUE;
	}

	static bool DisableWow64FsRedirection(PVOID* ctx)
	{
		typedef BOOL(WINAPI *LPFN_Wow64DisableWow64FsRedirection) (PVOID *OldValue);
		static auto func = (LPFN_Wow64DisableWow64FsRedirection)::GetProcAddress(::GetModuleHandle(L"kernel32"), "Wow64DisableWow64FsRedirection");
	
		if (!func)
			return false;
	
		return func(ctx) == TRUE;
	}

	static bool RevertWow64FsRedirection(PVOID ctx)
	{
		typedef BOOL(WINAPI *LPFN_Wow64RevertWow64FsRedirection) (PVOID OldValue);
		static auto func = (LPFN_Wow64RevertWow64FsRedirection)::GetProcAddress(::GetModuleHandle(L"kernel32"), "Wow64RevertWow64FsRedirection");

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
	si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
	si.hStdOutput = ::GetStdHandle(STD_OUTPUT_HANDLE);
	si.hStdError = hWritePipe;

	// CreateProcess requires a modifiable commandline parameter
	WCHAR cmdLine2[MAX_PATH];
	wsprintf(cmdLine2, cmdLine);

	ScopedDisableWow64FsRedirection a;

	if (!::CreateProcess(NULL, cmdLine2, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi))
	{
		debug_print("CreateProcess failed. err%#x, cmdline=%S\n", GetLastError(), cmdLine);
		return false;
	}

	::CloseHandle(pi.hProcess);
	::CloseHandle(pi.hThread);

	return true;
}

void ReadPipeLoop(HANDLE hReadPipe)
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
		if (!::PeekNamedPipe(hReadPipe, buffer.data(), 1, &num, &bytesEvailable, nullptr))
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
		debug_print("CreatePipe failed. err%#x\n", GetLastError());
		goto error;
	}

	if (!::SetHandleInformation(hWritePipe, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT))
	{
		debug_print("SetHandleInformation failed. err%#x\n", GetLastError());
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

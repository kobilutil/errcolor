#include "stdafx.h"
#include "utils.h"

// a helper macro that uses c++ compile-time type detection to ease the usage of the
// GetProcAddress api. For example, compare
// this:
//		typedef BOOL(WINAPI *LPFN_Wow64DisableWow64FsRedirection) (PVOID *OldValue);
//		static auto func = (LPFN_Wow64DisableWow64FsRedirection)::GetProcAddress(::GetModuleHandle(L"kernel32"), "Wow64DisableWow64FsRedirection");
// or this:
//		static auto func = (BOOL(WINAPI *)(PVOID *))::GetProcAddress(::GetModuleHandle(L"kernel32"), "Wow64DisableWow64FsRedirection");
// to that:
//		static auto func = GETPROC(Wow64DisableWow64FsRedirection, kernel32);
#define GETPROC(F,M) (decltype(&F))::GetProcAddress(::GetModuleHandle(L#M), #F)

void debug_print(char const* format, ...)
{
#ifdef _DEBUG
	va_list args;
	va_start(args, format);
	char line[200];
	::wvsprintfA(line, format, args);
	va_end(args);
	::OutputDebugStringA(line);
#endif
}

void file_print(HANDLE hFile, char const* format, ...)
{
	va_list args;
	va_start(args, format);
	char line[200];
	auto len = ::wvsprintfA(line, format, args);
	va_end(args);
	DWORD written;
	::WriteFile(hFile, line, len, &written, NULL);
}

// temporary disable the Wow64 file system redirection for 32bit process under 64bit OS
ScopedDisableWow64FsRedirection::ScopedDisableWow64FsRedirection()
{
	if (IsWow64())
		_redirected = DisableWow64FsRedirection(&_ctx);
}
ScopedDisableWow64FsRedirection::~ScopedDisableWow64FsRedirection()
{
	if (_redirected)
		RevertWow64FsRedirection(_ctx);
}

bool ScopedDisableWow64FsRedirection::IsWow64()
{
	static auto func = GETPROC(IsWow64Process, kernel32);
	if (!func)
		return false;

	BOOL bIsWow64;
	if (!func(::GetCurrentProcess(), &bIsWow64))
		return false;

	return bIsWow64 == TRUE;
}

bool ScopedDisableWow64FsRedirection::DisableWow64FsRedirection(PVOID* ctx)
{
	static auto func = GETPROC(Wow64DisableWow64FsRedirection, kernel32);
	if (!func)
		return false;

	return func(ctx) == TRUE;
}

bool ScopedDisableWow64FsRedirection::RevertWow64FsRedirection(PVOID ctx)
{
	static auto func = GETPROC(Wow64RevertWow64FsRedirection, kernel32);
	if (!func)
		return false;

	return func(ctx) == TRUE;
}

// default console's signal handler for use with SetConsoleCtrlHandler
static BOOL WINAPI DefaultCtrlHandler(DWORD CtrlType)
{
	switch (CtrlType)
	{
		// ignore ctrl-c and ctrl-break signals
	case CTRL_C_EVENT:
#ifndef _DEBUG 
		// for debug builds allow ctr-break to terminate the app
	case CTRL_BREAK_EVENT:
#endif
		return TRUE;
	}
	// for all other signals allow the default processing.
	// whithout this windows xp will display an ugly end-process dialog box
	// when the user tries to close the console window.
	debug_print("DefaultCtrlHandler: %d\n", CtrlType);
	return FALSE;
}

void InstallDefaultCtrlHandler()
{
	::SetConsoleCtrlHandler(DefaultCtrlHandler, TRUE);
}

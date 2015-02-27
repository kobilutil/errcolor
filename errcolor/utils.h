#pragma once

// a simple printf style wrapper for OutputDebugString
void debug_print(char const* format, ...);

void file_print(HANDLE hFile, char const* format, ...);

void InstallDefaultCtrlHandler();

// temporary disable the Wow64 file system redirection for 32bit process under 64bit OS
class ScopedDisableWow64FsRedirection
{
	// noncopyable
	ScopedDisableWow64FsRedirection(ScopedDisableWow64FsRedirection const&) = delete;
	ScopedDisableWow64FsRedirection& operator=(ScopedDisableWow64FsRedirection const&) = delete;

public:
	ScopedDisableWow64FsRedirection();
	~ScopedDisableWow64FsRedirection();

private:
	static bool IsWow64();
	static bool DisableWow64FsRedirection(PVOID* ctx);
	static bool RevertWow64FsRedirection(PVOID ctx);

private:
	bool	_redirected{};
	PVOID	_ctx{};
};

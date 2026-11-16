#pragma once

#include <windows.h>

#include <string>
#include <vector>

// Helpers for picking which application the audio relay listens to.

struct AudioAppInfo {
    DWORD processId = 0;
    std::wstring exeName;       // e.g. cod.exe
    std::wstring friendlyName;  // e.g. Call of Duty
    std::wstring windowTitle;
};

// Fills in exe and friendly name for a process. Returns false if the process cannot be opened.
bool DescribeProcess(DWORD processId, AudioAppInfo& info);

// Lists applications with a visible top-level window that are safe relay targets.
std::vector<AudioAppInfo> ListCapturableApplications();

// Finds a running process by exe name (case-insensitive). Prefers one that owns a visible window.
DWORD FindProcessByExeName(const std::wstring& exeName);

// True for Discord, Discord PTB/Canary/Development.
bool IsDiscordExe(const std::wstring& exeName);

// True for shell surfaces (taskbar, Start, search) that should never count as the "active application".
bool IsShellExe(const std::wstring& exeName);

// Returns an empty string when the process tree rooted at processId is safe to relay.
// Otherwise returns a user-facing reason (it would include Discord or HDR Corrector itself).
std::wstring AudioTargetProblem(DWORD processId);

std::wstring AudioAppDisplayName(const AudioAppInfo& info);

// Largest visible top-level window owned by the process, or nullptr.
HWND MainWindowForProcess(DWORD processId);

// 1 = elevated (Run as administrator), 0 = not elevated, -1 = could not tell (often means elevated/protected).
int ProcessElevation(DWORD processId);

// Windows build number (e.g. 22631). Per-app audio capture needs build 20348 or newer.
DWORD WindowsBuildNumber();

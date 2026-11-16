#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include "audio_source.h"

#include "platform.h"

#include <dwmapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cwctype>
#include <deque>
#include <filesystem>
#include <map>
#include <set>

namespace {

std::wstring Lower(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return text;
}

std::wstring ProcessImagePath(DWORD processId) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process) {
        return {};
    }

    wchar_t buffer[MAX_PATH * 2] = {};
    DWORD size = static_cast<DWORD>(std::size(buffer));
    std::wstring path;
    if (QueryFullProcessImageNameW(process, 0, buffer, &size)) {
        path.assign(buffer, size);
    }
    CloseHandle(process);
    return path;
}

std::wstring FileDescription(const std::wstring& path) {
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (size == 0) {
        return {};
    }

    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) {
        return {};
    }

    struct Translation {
        WORD language;
        WORD codePage;
    };
    Translation* translations = nullptr;
    UINT translationBytes = 0;
    std::vector<std::wstring> keys;
    if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&translations), &translationBytes)) {
        for (UINT i = 0; i < translationBytes / sizeof(Translation); ++i) {
            wchar_t key[64] = {};
            swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\FileDescription", translations[i].language, translations[i].codePage);
            keys.emplace_back(key);
        }
    }
    keys.emplace_back(L"\\StringFileInfo\\040904b0\\FileDescription");
    keys.emplace_back(L"\\StringFileInfo\\040904e4\\FileDescription");

    for (const auto& key : keys) {
        wchar_t* value = nullptr;
        UINT length = 0;
        if (VerQueryValueW(data.data(), key.c_str(), reinterpret_cast<void**>(&value), &length) && value && length > 1) {
            std::wstring text(value);
            while (!text.empty() && std::iswspace(text.back())) {
                text.pop_back();
            }
            if (!text.empty()) {
                return text;
            }
        }
    }
    return {};
}

bool IsAppWindow(HWND hwnd) {
    if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr) {
        return false;
    }
    if ((GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0) {
        return false;
    }
    if (GetWindowTextLengthW(hwnd) == 0) {
        return false;
    }
    BOOL cloaked = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) {
        return false;
    }
    return true;
}

std::wstring WindowTitle(HWND hwnd) {
    wchar_t buffer[256] = {};
    GetWindowTextW(hwnd, buffer, static_cast<int>(std::size(buffer)));
    return buffer;
}

// pid -> first visible app window title
std::map<DWORD, std::wstring> VisibleAppWindows() {
    std::map<DWORD, std::wstring> windows;
    EnumWindows(
        [](HWND hwnd, LPARAM param) -> BOOL {
            auto* result = reinterpret_cast<std::map<DWORD, std::wstring>*>(param);
            if (!IsAppWindow(hwnd)) {
                return TRUE;
            }
            DWORD processId = 0;
            GetWindowThreadProcessId(hwnd, &processId);
            if (processId != 0 && result->find(processId) == result->end()) {
                (*result)[processId] = WindowTitle(hwnd);
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&windows));
    return windows;
}

struct ProcessEntry {
    DWORD parentId = 0;
    std::wstring exeName;
};

std::map<DWORD, ProcessEntry> ProcessSnapshot() {
    std::map<DWORD, ProcessEntry> processes;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return processes;
    }
    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            processes[entry.th32ProcessID] = {entry.th32ParentProcessID, entry.szExeFile};
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return processes;
}

ULONGLONG ProcessCreationTime(DWORD processId) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process) {
        return 0;
    }
    FILETIME created = {}, exited = {}, kernel = {}, user = {};
    ULONGLONG value = 0;
    if (GetProcessTimes(process, &created, &exited, &kernel, &user)) {
        value = (static_cast<ULONGLONG>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    }
    CloseHandle(process);
    return value;
}

std::wstring CheckTree(DWORD rootId, const std::map<DWORD, ProcessEntry>& processes) {
    if (rootId == 0 || rootId == 4) {
        return L"System processes cannot be relayed.";
    }

    const DWORD self = GetCurrentProcessId();
    std::map<DWORD, std::vector<DWORD>> children;
    for (const auto& [processId, entry] : processes) {
        if (entry.parentId != 0 && entry.parentId != processId) {
            children[entry.parentId].push_back(processId);
        }
    }

    // Walk every process Windows would count as part of the target's tree.
    std::deque<DWORD> pending = {rootId};
    std::set<DWORD> visited;
    while (!pending.empty()) {
        const DWORD current = pending.front();
        pending.pop_front();
        if (!visited.insert(current).second) {
            continue;
        }

        if (current == self) {
            return L"That application started HDR Corrector, so relaying it would feed the relay back into itself.";
        }
        const auto found = processes.find(current);
        if (found != processes.end() && IsDiscordExe(found->second.exeName)) {
            return current == rootId
                ? L"Discord cannot be used as the audio source (it would echo voice chat)."
                : L"That application launched Discord, so relaying it would include Discord voice chat.";
        }

        const auto kids = children.find(current);
        if (kids == children.end()) {
            continue;
        }
        const ULONGLONG parentCreated = ProcessCreationTime(current);
        for (const DWORD child : kids->second) {
            // A parent PID can be stale and reused by an unrelated, newer process. Skip those links.
            const ULONGLONG childCreated = ProcessCreationTime(child);
            if (parentCreated != 0 && childCreated != 0 && childCreated < parentCreated) {
                continue;
            }
            pending.push_back(child);
        }
    }
    return {};
}

}  // namespace

bool IsDiscordExe(const std::wstring& exeName) {
    const auto name = Lower(exeName);
    return name == L"discord.exe" || name == L"discordptb.exe" || name == L"discordcanary.exe" ||
        name == L"discorddevelopment.exe";
}

bool IsShellExe(const std::wstring& exeName) {
    static const wchar_t* const kShell[] = {
        L"explorer.exe",
        L"startmenuexperiencehost.exe",
        L"searchhost.exe",
        L"searchapp.exe",
        L"shellexperiencehost.exe",
        L"textinputhost.exe",
        L"lockapp.exe",
        L"applicationframehost.exe",
        L"shellhost.exe",
    };
    const auto name = Lower(exeName);
    return std::any_of(std::begin(kShell), std::end(kShell), [&](const wchar_t* shell) { return name == shell; });
}

bool DescribeProcess(DWORD processId, AudioAppInfo& info) {
    const auto path = ProcessImagePath(processId);
    if (path.empty()) {
        return false;
    }
    info.processId = processId;
    info.exeName = std::filesystem::path(path).filename().wstring();
    info.friendlyName = FileDescription(path);
    return true;
}

std::wstring AudioAppDisplayName(const AudioAppInfo& info) {
    if (!info.friendlyName.empty()) {
        return info.friendlyName;
    }
    if (!info.windowTitle.empty()) {
        return info.windowTitle;
    }
    return info.exeName;
}

std::vector<AudioAppInfo> ListCapturableApplications() {
    std::vector<AudioAppInfo> apps;
    const auto windows = VisibleAppWindows();
    const auto processes = ProcessSnapshot();
    const DWORD self = GetCurrentProcessId();

    for (const auto& [processId, title] : windows) {
        if (processId == self) {
            continue;
        }
        AudioAppInfo info;
        if (!DescribeProcess(processId, info)) {
            continue;
        }
        info.windowTitle = title;
        if (IsDiscordExe(info.exeName) || IsShellExe(info.exeName)) {
            continue;
        }
        const auto problem = CheckTree(processId, processes);
        if (!problem.empty()) {
            Log(L"Audio picker hiding " + info.exeName + L" (PID " + std::to_wstring(processId) + L"): " + problem);
            continue;
        }
        apps.push_back(std::move(info));
    }

    std::sort(apps.begin(), apps.end(), [](const AudioAppInfo& a, const AudioAppInfo& b) {
        return Lower(AudioAppDisplayName(a)) < Lower(AudioAppDisplayName(b));
    });
    return apps;
}

DWORD FindProcessByExeName(const std::wstring& exeName) {
    if (exeName.empty()) {
        return 0;
    }
    const auto wanted = Lower(exeName);
    const auto windows = VisibleAppWindows();
    const auto processes = ProcessSnapshot();
    DWORD fallback = 0;
    for (const auto& [processId, entry] : processes) {
        if (Lower(entry.exeName) != wanted || processId == GetCurrentProcessId()) {
            continue;
        }
        if (windows.find(processId) != windows.end()) {
            return processId;
        }
        // Without a window yet, prefer the top-most copy so its children are covered too.
        const auto parent = processes.find(entry.parentId);
        const bool parentIsSameApp = parent != processes.end() && Lower(parent->second.exeName) == wanted;
        if (fallback == 0 || !parentIsSameApp) {
            fallback = processId;
        }
    }
    return fallback;
}

std::wstring AudioTargetProblem(DWORD processId) {
    return CheckTree(processId, ProcessSnapshot());
}

DWORD WindowsBuildNumber() {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto rtlGetVersion = ntdll ? reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion")) : nullptr;
    if (!rtlGetVersion) {
        return 0;
    }
    OSVERSIONINFOW info = {};
    info.dwOSVersionInfoSize = sizeof(info);
    return rtlGetVersion(&info) == 0 ? info.dwBuildNumber : 0;
}

HWND MainWindowForProcess(DWORD processId) {
    struct Search {
        DWORD processId;
        HWND best;
        LONG bestArea;
    } search = {processId, nullptr, -1};

    EnumWindows(
        [](HWND hwnd, LPARAM param) -> BOOL {
            auto* state = reinterpret_cast<Search*>(param);
            DWORD owner = 0;
            GetWindowThreadProcessId(hwnd, &owner);
            if (owner != state->processId || !IsAppWindow(hwnd)) {
                return TRUE;
            }
            RECT rect = {};
            GetWindowRect(hwnd, &rect);
            const LONG area = (rect.right - rect.left) * (rect.bottom - rect.top);
            if (area > state->bestArea) {
                state->best = hwnd;
                state->bestArea = area;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));
    return search.best;
}

int ProcessElevation(DWORD processId) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process) {
        return -1;
    }
    HANDLE token = nullptr;
    int result = -1;
    if (OpenProcessToken(process, TOKEN_QUERY, &token)) {
        TOKEN_ELEVATION elevation = {};
        DWORD size = 0;
        if (GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size)) {
            result = elevation.TokenIsElevated ? 1 : 0;
        }
        CloseHandle(token);
    }
    CloseHandle(process);
    return result;
}

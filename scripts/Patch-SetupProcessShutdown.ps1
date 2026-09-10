param([Parameter(Mandatory=$true)][string]$SetupSource)
$ErrorActionPreference='Stop'
$s=Get-Content $SetupSource -Raw
$s=$s.Replace('#include <windows.h>',"#include <windows.h>`r`n#include <tlhelp32.h>")
$anchor='static bool LaunchVerify(const fs::path& root, bool ciMode, std::wstring& error) {'
if(-not $s.Contains($anchor)){throw 'LaunchVerify anchor missing'}
$helper=@'
static bool IsPathInsideSetup(const fs::path& candidate, const fs::path& directory) {
    std::error_code ec;
    const auto value = fs::absolute(candidate, ec).lexically_normal().wstring();
    if (ec) return false;
    ec.clear();
    auto prefix = fs::absolute(directory, ec).lexically_normal().wstring();
    if (ec) return false;
    while (!prefix.empty() && (prefix.back() == L'\\' || prefix.back() == L'/')) prefix.pop_back();
    prefix.push_back(L'\\');
    return value.size() >= prefix.size() && _wcsnicmp(value.c_str(), prefix.c_str(), prefix.size()) == 0;
}
static std::vector<DWORD> OwnedLauncherProcessIds(const fs::path& root) {
    std::vector<DWORD> result;
    std::error_code ec;
    const auto host = fs::absolute(root / L"SyntexLauncher.exe", ec).lexically_normal();
    if (ec) return result;
    ec.clear();
    const auto app = fs::absolute(root / L"app", ec).lexically_normal();
    if (ec) return result;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return result;
    PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ProcessID == 0 || entry.th32ProcessID == GetCurrentProcessId()) continue;
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
            if (!process) continue;
            std::wstring image(32768, L'\0'); DWORD imageLength = static_cast<DWORD>(image.size());
            if (QueryFullProcessImageNameW(process, 0, image.data(), &imageLength) && imageLength > 0) {
                image.resize(imageLength);
                const fs::path processPath = fs::path(image).lexically_normal();
                if (_wcsicmp(processPath.c_str(), host.c_str()) == 0 || IsPathInsideSetup(processPath, app)) result.push_back(entry.th32ProcessID);
            }
            CloseHandle(process);
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return result;
}
static void RequestCloseForPid(DWORD pid) {
    struct Context { DWORD pid; } context{pid};
    EnumWindows([](HWND hwnd, LPARAM value) -> BOOL {
        auto* context = reinterpret_cast<Context*>(value);
        DWORD windowPid = 0; GetWindowThreadProcessId(hwnd, &windowPid);
        if (windowPid == context->pid) PostMessageW(hwnd, WM_CLOSE, 0, 0);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&context));
}
static bool StopLauncherProgramProcesses(const fs::path& root, std::wstring& error) noexcept {
    try {
        for (const DWORD pid : OwnedLauncherProcessIds(root)) RequestCloseForPid(pid);
        const ULONGLONG gracefulDeadline = GetTickCount64() + 5000;
        while (GetTickCount64() < gracefulDeadline) { if (OwnedLauncherProcessIds(root).empty()) return true; Sleep(100); }
        const ULONGLONG hardDeadline = GetTickCount64() + 10000;
        while (GetTickCount64() < hardDeadline) {
            const auto pids = OwnedLauncherProcessIds(root);
            if (pids.empty()) return true;
            for (const DWORD pid : pids) {
                HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
                if (!process) continue;
                TerminateProcess(process, 206); WaitForSingleObject(process, 2000); CloseHandle(process);
            }
            Sleep(100);
        }
        if (!OwnedLauncherProcessIds(root).empty()) {
            error = L"Syntex Launcher konnte für Installation/Aktualisierung nicht vollständig beendet werden.";
            return false;
        }
        return true;
    } catch (...) {
        error = L"Laufende Syntex-Prozesse konnten vor Installation/Aktualisierung nicht zuverlässig beendet werden.";
        return false;
    }
}

'@
$s=$s.Replace($anchor,$helper+$anchor)
$old='const std::wstring command = Quote(host.wstring()) + (ciMode ? L" --ci-gui-self-test" : L"");'
if(-not $s.Contains($old)){throw 'LaunchVerify command anchor missing'}
$s=$s.Replace($old,'const std::wstring command = Quote(host.wstring()) + L" --post-update-health-check";')
$old2='if (!ValidateWritableRoot(root, error)) { cleanup(); return false; }'
if(-not $s.Contains($old2)){throw 'Writable root anchor missing'}
$s=$s.Replace($old2,$old2+"`r`n`r`n    progress(6, L`"Laufender Syntex Launcher wird für Installation/Aktualisierung beendet …`" );`r`n    if (!StopLauncherProgramProcesses(root, error)) { cleanup(); return false; }")
$downloadPattern='unsigned long long total = 0;\s*DWORD available = 0;\s*while \(WinHttpQueryDataAvailable\(request, &available\) && available > 0\) \{'
if(-not [regex]::IsMatch($s,$downloadPattern)){throw 'Download loop anchor missing'}
$downloadReplacement=@'
unsigned long long total = 0;
    DWORD available = 0;
    const ULONGLONG downloadDeadline = GetTickCount64() + 300000ULL;
    while (WinHttpQueryDataAvailable(request, &available) && available > 0) {
        if (GetTickCount64() > downloadDeadline) {
            error = L"Launcher-Download hat das maximale Zeitlimit von 5 Minuten überschritten.";
            f.close();
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connect);
            WinHttpCloseHandle(session);
            return false;
        }
'@
$s=[regex]::Replace($s,$downloadPattern,$downloadReplacement,1)
$extract='if (!RunProcess(Quote(tar.wstring()) + L" -xf " + Quote(zip.wstring()) + L" -C " + Quote(target.wstring()), target, code, error)) return false;'
if(-not $s.Contains($extract)){throw 'Extract anchor missing'}
$s=$s.Replace($extract,'if (!RunProcess(Quote(tar.wstring()) + L" -xf " + Quote(zip.wstring()) + L" -C " + Quote(target.wstring()), target, code, error, 120000)) return false;')
$s=$s.Replace('Syntex Launcher Setup 0.16.6 RC1','Syntex Launcher Setup 0.16.7')
$s=$s.Replace('Syntex Launcher Setup 0.16.6','Syntex Launcher Setup 0.16.7')
$s=$s.Replace('SyntexLauncherSetup/0.16.6-RC1','SyntexLauncherSetup/0.16.7')
$s=$s.Replace('DisplayVersion", L"0.16.6"','DisplayVersion", L"0.16.7"')
Set-Content $SetupSource $s -Encoding utf8
Write-Host 'SETUP_DETERMINISTIC_PROCESS_DRAIN_AND_BOUNDED_IO_PATCH_PASS'

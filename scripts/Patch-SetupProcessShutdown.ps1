param([Parameter(Mandatory=$true)][string]$SetupSource)
$ErrorActionPreference='Stop'
$s=Get-Content $SetupSource -Raw
$s=$s.Replace('#include <windows.h>',"#include <windows.h>`r`n#include <tlhelp32.h>")

$httpAnchor='static bool HttpDownload(const std::wstring& url, const fs::path& target, unsigned long long expectedSize, std::wstring& error, bool reportProgress) {'
if(-not $s.Contains($httpAnchor)){throw 'HttpDownload anchor missing'}
$httpPrefix=@'
static fs::path gCiPayloadSource;

static bool HttpDownload(const std::wstring& url, const fs::path& target, unsigned long long expectedSize, std::wstring& error, bool reportProgress) {
    if (!gCiPayloadSource.empty()) {
        std::error_code ec;
        fs::copy_file(gCiPayloadSource, target, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            error = L"Lokales CI-Payload konnte nicht bereitgestellt werden.";
            return false;
        }
        if (expectedSize && fs::file_size(target, ec) != expectedSize) {
            error = L"Lokales CI-Payload hat eine unerwartete Dateigröße.";
            return false;
        }
        return true;
    }
'@
$s=$s.Replace($httpAnchor,$httpPrefix)

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
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ProcessID == 0 || entry.th32ProcessID == GetCurrentProcessId()) continue;
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
            if (!process) continue;
            std::wstring image(32768, L'\0');
            DWORD imageLength = static_cast<DWORD>(image.size());
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

static bool StopLauncherProgramProcesses(const fs::path& root, std::wstring& error) noexcept {
    try {
        const ULONGLONG deadline = GetTickCount64() + 10000;
        while (GetTickCount64() < deadline) {
            const auto pids = OwnedLauncherProcessIds(root);
            if (pids.empty()) return true;
            for (const DWORD pid : pids) {
                HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
                if (!process) continue;
                TerminateProcess(process, 206);
                WaitForSingleObject(process, 2000);
                CloseHandle(process);
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

// GUI health checks must not use CREATE_NO_WINDOW. RunProcess intentionally hides console
// helpers such as tar.exe, but the launcher health check must run in the normal GUI desktop.
static bool RunLauncherHealthProcess(const std::wstring& command, const fs::path& cwd, DWORD& exitCode, std::wstring& error, DWORD timeout) {
    std::vector<wchar_t> mutableCmd(command.begin(), command.end());
    mutableCmd.push_back(L'\0');
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE, 0, nullptr,
            cwd.empty() ? nullptr : cwd.c_str(), &si, &pi)) {
        error = L"Windows konnte den Launcher-Startwächter nicht starten. Fehler " + std::to_wstring(GetLastError()) + L".";
        return false;
    }
    CloseHandle(pi.hThread);
    const DWORD wait = WaitForSingleObject(pi.hProcess, timeout);
    if (wait == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 124);
        WaitForSingleObject(pi.hProcess, 2000);
        CloseHandle(pi.hProcess);
        error = L"Launcher-Startwächter hat das Zeitlimit überschritten.";
        return false;
    }
    if (wait != WAIT_OBJECT_0) {
        CloseHandle(pi.hProcess);
        error = L"Launcher-Startwächter konnte nicht überwacht werden.";
        return false;
    }
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    return true;
}

'@
$s=$s.Replace($anchor,$helper+$anchor)
$old='const std::wstring command = Quote(host.wstring()) + (ciMode ? L" --ci-gui-self-test" : L"");'
if(-not $s.Contains($old)){throw 'LaunchVerify command anchor missing'}
$s=$s.Replace($old,'const std::wstring command = Quote(host.wstring()) + L" --post-update-health-check";')
$oldRun='if (!RunProcess(command, root, code, error, 135000)) {'
if(-not $s.Contains($oldRun)){throw 'LaunchVerify RunProcess anchor missing'}
$s=$s.Replace($oldRun,'if (!RunLauncherHealthProcess(command, root, code, error, 135000)) {')

$pattern='progress\(87, L"Vorhandene Programmdateien werden transaktional gesichert …"\);\s*if \(!SyntexSetupRepair::Begin\(root, stage, transaction, error\)\) \{ cleanup\(\); return false; \}'
if(-not [regex]::IsMatch($s,$pattern)){throw 'Repair transaction anchor missing'}
$replacement=@'
progress(86, L"Laufender Syntex Launcher wird für Installation/Aktualisierung beendet …");
    if (!StopLauncherProgramProcesses(root, error)) { cleanup(); return false; }

    progress(87, L"Vorhandene Programmdateien werden transaktional gesichert …");
    if (!SyntexSetupRepair::Begin(root, stage, transaction, error)) { cleanup(); return false; }
'@
$s=[regex]::Replace($s,$pattern,$replacement,1)

$ciAnchor='    if (argv && argc == 3 && _wcsicmp(argv[1], L"--ci-install") == 0) {'
if(-not $s.Contains($ciAnchor)){throw 'CI install anchor missing'}
$ciModes=@'
    if (argv && argc == 3 && _wcsicmp(argv[1], L"--ci-stop-launcher") == 0) {
        const fs::path target(argv[2]);
        LocalFree(argv);
        std::wstring stopError;
        return StopLauncherProgramProcesses(target, stopError) ? 0 : 6;
    }
    if (argv && argc == 4 && _wcsicmp(argv[1], L"--ci-install-local") == 0) {
        const fs::path target(argv[2]);
        gCiPayloadSource = fs::path(argv[3]);
        LocalFree(argv);
        bool repair = false;
        std::wstring error;
        const bool ok = InstallCore(target, true, false, repair, error);
        gCiPayloadSource.clear();
        if (!ok) { WriteSetupLog(L"CI local install E2E: " + error); return 7; }
        return 0;
    }
'@
$s=$s.Replace($ciAnchor,$ciModes+$ciAnchor)

$s=$s.Replace('Syntex Launcher Setup 0.16.6 RC1','Syntex Launcher Setup 0.16.7')
$s=$s.Replace('Syntex Launcher Setup 0.16.6','Syntex Launcher Setup 0.16.7')
$s=$s.Replace('SyntexLauncherSetup/0.16.6-RC1','SyntexLauncherSetup/0.16.7')
$s=$s.Replace('DisplayVersion", L"0.16.6"','DisplayVersion", L"0.16.7"')
Set-Content $SetupSource $s -Encoding utf8
Write-Host 'SETUP_FORCE_PROCESS_DRAIN_VISIBLE_HEALTH_LOCAL_PAYLOAD_TEST_PATCH_PASS'

param([Parameter(Mandatory=$true)][string]$SetupSource)
$ErrorActionPreference='Stop'
$s=Get-Content $SetupSource -Raw

$pattern='static bool StopLauncherProgramProcesses\(const fs::path& root, std::wstring& error\) noexcept \{.*?\r?\n\}\r?\n\r?\n// GUI health checks must not use CREATE_NO_WINDOW\.'
if(-not [regex]::IsMatch($s,$pattern,[System.Text.RegularExpressions.RegexOptions]::Singleline)){
    throw 'Force-only StopLauncherProgramProcesses block not found'
}

$replacement=@'
static void RequestCloseForPid(DWORD pid) noexcept {
    struct CloseContext { DWORD pid; } context{ pid };
    EnumWindows([](HWND hwnd, LPARAM value) -> BOOL {
        auto* ctx = reinterpret_cast<CloseContext*>(value);
        DWORD windowPid = 0;
        GetWindowThreadProcessId(hwnd, &windowPid);
        if (windowPid == ctx->pid) {
            DWORD_PTR ignored = 0;
            SendMessageTimeoutW(
                hwnd,
                WM_CLOSE,
                0,
                0,
                SMTO_ABORTIFHUNG | SMTO_BLOCK,
                1000,
                &ignored);
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&context));
}

static bool StopLauncherProgramProcesses(const fs::path& root, std::wstring& error) noexcept {
    try {
        auto pids = OwnedLauncherProcessIds(root);
        if (pids.empty()) return true;

        // First request a normal application shutdown. This lets the launcher flush
        // settings/logs and avoids creating a dirty next-start state.
        for (const DWORD pid : pids) RequestCloseForPid(pid);

        // Match the proven lifecycle gate: allow up to 15 seconds for a complete,
        // clean shutdown before any exact-root force fallback is considered.
        const ULONGLONG gracefulDeadline = GetTickCount64() + 15000;
        while (GetTickCount64() < gracefulDeadline) {
            if (OwnedLauncherProcessIds(root).empty()) return true;
            Sleep(100);
        }

        // Only exact-root processes still alive after the graceful deadline are forced.
        const ULONGLONG forceDeadline = GetTickCount64() + 10000;
        while (GetTickCount64() < forceDeadline) {
            pids = OwnedLauncherProcessIds(root);
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

// GUI health checks must not use CREATE_NO_WINDOW.
'@

$s=[regex]::Replace(
    $s,
    $pattern,
    $replacement,
    [System.Text.RegularExpressions.RegexOptions]::Singleline)
Set-Content $SetupSource $s -Encoding utf8
Write-Host 'SETUP_GRACEFUL_THEN_FORCE_EXACT_ROOT_SHUTDOWN_PATCH_PASS'

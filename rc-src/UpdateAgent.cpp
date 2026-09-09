#define UNICODE
#define _UNICODE
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <system_error>

namespace fs = std::filesystem;

static std::wstring Timestamp() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t b[64]{};
    swprintf_s(b, L"%04u%02u%02u-%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return b;
}

static std::string Utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n, nullptr, nullptr);
    return out;
}

static fs::path ExePath() {
    std::wstring b(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, b.data(), static_cast<DWORD>(b.size()));
    if (n == 0 || n >= b.size()) return {};
    b.resize(n);
    return fs::path(b);
}

static fs::path WriteLog(const fs::path& root, const std::wstring& text) noexcept {
    try {
        std::error_code ec;
        const fs::path dir = root / L"storage" / L"logs" / L"launcher-update";
        fs::create_directories(dir, ec);
        if (ec) return {};
        const fs::path path = dir / (L"SyntexUpdate-" + Timestamp() + L"-" + std::to_wstring(GetCurrentProcessId()) + L".log");
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f) return {};
        const auto u8 = Utf8(text);
        f.write(u8.data(), static_cast<std::streamsize>(u8.size()));
        return path;
    } catch (...) {
        return {};
    }
}

static std::wstring Quote(const std::wstring& value) {
    return L"\"" + value + L"\"";
}

static std::wstring ArgValue(const std::vector<std::wstring>& args, const wchar_t* name) {
    for (size_t i = 0; i + 1 < args.size(); ++i) {
        if (_wcsicmp(args[i].c_str(), name) == 0) return args[i + 1];
    }
    return {};
}

static bool WaitForParent(DWORD pid, std::wstring& error) {
    if (pid == 0) return true;
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!process) {
        const DWORD e = GetLastError();
        if (e == ERROR_INVALID_PARAMETER) return true;
        error = L"Der laufende Launcher konnte nicht für die Aktualisierung überwacht werden. Windows-Fehler: " + std::to_wstring(e);
        return false;
    }
    const DWORD wait = WaitForSingleObject(process, 60000);
    CloseHandle(process);
    if (wait == WAIT_OBJECT_0) return true;
    error = wait == WAIT_TIMEOUT
        ? L"Der bisherige Launcher wurde nicht innerhalb von 60 Sekunden beendet."
        : L"Der bisherige Launcher konnte nicht zuverlässig beendet werden.";
    return false;
}

static bool CopyTree(const fs::path& source, const fs::path& target, std::wstring& error) {
    std::error_code ec;
    if (!fs::exists(source)) {
        error = L"Update-Datei fehlt: " + source.wstring();
        return false;
    }
    fs::create_directories(target.parent_path(), ec);
    ec.clear();
    fs::copy(source, target,
        fs::copy_options::recursive | fs::copy_options::overwrite_existing | fs::copy_options::copy_symlinks,
        ec);
    if (ec) {
        error = L"Dateien konnten nicht kopiert werden: " + source.wstring() + L" -> " + target.wstring() + L" (" + std::to_wstring(ec.value()) + L")";
        return false;
    }
    return true;
}

static bool BackupCurrent(const fs::path& root, const fs::path& rollback, std::wstring& error) {
    std::error_code ec;
    fs::remove_all(rollback, ec);
    ec.clear();
    fs::create_directories(rollback, ec);
    if (ec) {
        error = L"Rollback-Verzeichnis konnte nicht erstellt werden.";
        return false;
    }

    const fs::path host = root / L"SyntexLauncher.exe";
    const fs::path app = root / L"app";
    if (!fs::exists(host) || !fs::exists(app)) {
        error = L"Die bestehende Installation ist unvollständig und kann nicht sicher gesichert werden.";
        return false;
    }
    if (!CopyTree(host, rollback / L"SyntexLauncher.exe", error)) return false;
    if (!CopyTree(app, rollback / L"app", error)) return false;
    return true;
}

static bool InstallStage(const fs::path& root, const fs::path& stage, std::wstring& error) {
    const fs::path newHost = stage / L"SyntexLauncher.exe";
    const fs::path newApp = stage / L"app";
    const fs::path managed = newApp / L"SyntexLauncher.exe";
    const fs::path uninstall = newApp / L"SyntexUninstall.exe";
    const fs::path updater = newApp / L"SyntexUpdateAgent.exe";
    if (!fs::exists(newHost) || !fs::exists(managed) || !fs::exists(uninstall) || !fs::exists(updater)) {
        error = L"Das neue Launcher-Paket ist unvollständig.";
        return false;
    }

    std::error_code ec;
    fs::remove_all(root / L"app", ec);
    if (ec) {
        error = L"Alte Programmdateien konnten nicht entfernt werden. Möglicherweise läuft noch ein Syntex-Prozess.";
        return false;
    }
    if (!CopyTree(newApp, root / L"app", error)) return false;

    ec.clear();
    fs::copy_file(newHost, root / L"SyntexLauncher.exe", fs::copy_options::overwrite_existing, ec);
    if (ec) {
        error = L"SyntexLauncher.exe konnte nicht aktualisiert werden (" + std::to_wstring(ec.value()) + L").";
        return false;
    }
    return true;
}

static bool RestoreRollback(const fs::path& root, const fs::path& rollback, std::wstring& error) {
    if (!fs::exists(rollback / L"SyntexLauncher.exe") || !fs::exists(rollback / L"app")) {
        error = L"Rollback-Dateien fehlen.";
        return false;
    }
    std::error_code ec;
    fs::remove_all(root / L"app", ec);
    ec.clear();
    if (!CopyTree(rollback / L"app", root / L"app", error)) return false;
    fs::copy_file(rollback / L"SyntexLauncher.exe", root / L"SyntexLauncher.exe", fs::copy_options::overwrite_existing, ec);
    if (ec) {
        error = L"Die vorherige SyntexLauncher.exe konnte nicht wiederhergestellt werden.";
        return false;
    }
    return true;
}

static bool LaunchAndWait(const fs::path& exe, const std::wstring& arguments, DWORD timeoutMs, DWORD& exitCode, std::wstring& error) {
    std::wstring command = Quote(exe.wstring());
    if (!arguments.empty()) command += L" " + arguments;
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE, 0, nullptr, exe.parent_path().c_str(), &si, &pi)) {
        error = L"Der aktualisierte Launcher konnte nicht gestartet werden. Windows-Fehler: " + std::to_wstring(GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    const DWORD wait = WaitForSingleObject(pi.hProcess, timeoutMs);
    if (wait == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 120);
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hProcess);
        error = L"Die Startprüfung der neuen Launcher-Version hat das Zeitlimit überschritten.";
        return false;
    }
    if (wait != WAIT_OBJECT_0) {
        CloseHandle(pi.hProcess);
        error = L"Die neue Launcher-Version konnte nicht überwacht werden.";
        return false;
    }
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    return true;
}

static void LaunchDetached(const fs::path& exe, const std::wstring& arguments) {
    std::wstring command = Quote(exe.wstring());
    if (!arguments.empty()) command += L" " + arguments;
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(exe.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE, 0, nullptr, exe.parent_path().c_str(), &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* raw = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args;
    if (raw) {
        for (int i = 1; i < argc; ++i) args.emplace_back(raw[i]);
        LocalFree(raw);
    }

    const fs::path root = ArgValue(args, L"--root");
    const fs::path stage = ArgValue(args, L"--stage");
    const std::wstring parentText = ArgValue(args, L"--parent-pid");
    const std::wstring fromVersion = ArgValue(args, L"--from-version");
    const std::wstring toVersion = ArgValue(args, L"--to-version");
    const DWORD parentPid = parentText.empty() ? 0 : static_cast<DWORD>(_wcstoui64(parentText.c_str(), nullptr, 10));

    if (root.empty() || stage.empty()) return 201;

    std::wstring error;
    if (!WaitForParent(parentPid, error)) {
        WriteLog(root, L"UPDATE_ABORT\r\n" + error + L"\r\n");
        return 202;
    }

    const fs::path rollback = root / L"storage" / L"cache" / L"launcher-update" / L"rollback";
    if (!BackupCurrent(root, rollback, error)) {
        WriteLog(root, L"UPDATE_BACKUP_FAILED\r\n" + error + L"\r\n");
        return 203;
    }

    if (!InstallStage(root, stage, error)) {
        std::wstring restoreError;
        const bool restored = RestoreRollback(root, rollback, restoreError);
        WriteLog(root, L"UPDATE_INSTALL_FAILED\r\n" + error + L"\r\nRollback: " + (restored ? L"erfolgreich" : L"fehlgeschlagen: " + restoreError) + L"\r\n");
        if (restored) LaunchDetached(root / L"SyntexLauncher.exe", L"--update-rollback-notice");
        return 204;
    }

    DWORD healthExit = 0;
    if (!LaunchAndWait(root / L"SyntexLauncher.exe", L"--post-update-health-check", 120000, healthExit, error) || healthExit != 0) {
        std::wstring restoreError;
        const bool restored = RestoreRollback(root, rollback, restoreError);
        WriteLog(root,
            L"UPDATE_HEALTHCHECK_FAILED\r\nVon: " + fromVersion + L"\r\nNach: " + toVersion +
            L"\r\nExitcode: " + std::to_wstring(healthExit) + L"\r\n" + error +
            L"\r\nRollback: " + (restored ? L"erfolgreich" : L"fehlgeschlagen: " + restoreError) + L"\r\n");
        if (restored) LaunchDetached(root / L"SyntexLauncher.exe", L"--update-rollback-notice");
        return 205;
    }

    WriteLog(root,
        L"UPDATE_SUCCESS\r\nVon: " + fromVersion + L"\r\nNach: " + toVersion +
        L"\r\nPersönliche Daten wurden nicht ersetzt.\r\nRollback-Sicherung: " + rollback.wstring() + L"\r\n");

    std::error_code ec;
    fs::remove_all(stage, ec);
    const fs::path self = ExePath();
    if (!self.empty()) MoveFileExW(self.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    return 0;
}

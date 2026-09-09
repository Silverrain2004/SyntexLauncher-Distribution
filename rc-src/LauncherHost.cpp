#define UNICODE
#define _UNICODE
#include <windows.h>
#include <shellapi.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <chrono>
#include <thread>

namespace fs = std::filesystem;

static fs::path ExePath() {
    std::wstring buffer(32768, L'\0');
    DWORD len = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (len == 0 || len >= buffer.size()) return {};
    buffer.resize(len);
    return fs::path(buffer);
}

static std::wstring Timestamp() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t b[64]{};
    swprintf_s(b, L"%04u%02u%02u-%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return b;
}

static std::string Utf8(const std::wstring& text) {
    if (text.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n, nullptr, nullptr);
    return out;
}

static fs::path WriteLog(const fs::path& root, const std::wstring& text) {
    std::error_code ec;
    fs::path dir = root / L"storage" / L"logs" / L"launcher-startup";
    fs::create_directories(dir, ec);
    if (ec) return {};
    fs::path path = dir / (L"SyntexLauncher-Start-" + Timestamp() + L"-" + std::to_wstring(GetCurrentProcessId()) + L".log");
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return {};
    auto u8 = Utf8(text);
    f.write(u8.data(), static_cast<std::streamsize>(u8.size()));
    return path;
}

static std::wstring WinError(DWORD code) {
    wchar_t* msg = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<LPWSTR>(&msg), 0, nullptr);
    std::wstring result = n && msg ? std::wstring(msg, n) : L"Unbekannter Windows-Fehler";
    if (msg) LocalFree(msg);
    return result;
}

struct WindowProbe { DWORD pid; bool found; };
static BOOL CALLBACK EnumWindowProc(HWND hwnd, LPARAM lp) {
    auto* probe = reinterpret_cast<WindowProbe*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == probe->pid && IsWindowVisible(hwnd) && GetWindow(hwnd, GW_OWNER) == nullptr) {
        wchar_t title[2]{};
        if (GetWindowTextW(hwnd, title, 2) > 0) {
            probe->found = true;
            return FALSE;
        }
    }
    return TRUE;
}

static bool HasVisibleWindow(DWORD pid) {
    WindowProbe probe{pid, false};
    EnumWindows(EnumWindowProc, reinterpret_cast<LPARAM>(&probe));
    return probe.found;
}

static std::wstring Quote(const std::wstring& value) { return L"\"" + value + L"\""; }

static std::vector<std::wstring> Arguments() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> result;
    if (argv) {
        for (int i = 1; i < argc; ++i) result.emplace_back(argv[i]);
        LocalFree(argv);
    }
    return result;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    fs::path exe = ExePath();
    if (exe.empty()) return 90;
    fs::path root = exe.parent_path();
    fs::path child = root / L"app" / L"SyntexLauncher.App.exe";
    auto args = Arguments();

    if (args.size() == 1 && _wcsicmp(args[0].c_str(), L"--host-log-self-test") == 0) {
        auto log = WriteLog(root, L"Syntex Launcher 0.16.6 RC native startup logging self-test\r\n");
        return log.empty() ? 91 : 0;
    }

    const bool ciGui = args.size() == 1 && _wcsicmp(args[0].c_str(), L"--ci-gui-self-test") == 0;
    if (!fs::exists(child)) {
        auto log = WriteLog(root, L"Startfehler: app\\SyntexLauncher.App.exe fehlt.\r\nRoot: " + root.wstring() + L"\r\n");
        MessageBoxW(nullptr, (L"Syntex Launcher ist unvollständig.\n\nDiagnose: " + log.wstring()).c_str(), L"Syntex Launcher – Startfehler", MB_OK | MB_ICONERROR);
        return 92;
    }

    std::error_code ec;
    fs::create_directories(root / L"storage", ec);
    SetEnvironmentVariableW(L"SYNTEX_ROOT", (root / L"storage").c_str());

    std::wstring command = Quote(child.wstring());
    if (!ciGui) {
        for (const auto& arg : args) command += L" " + Quote(arg);
    }
    std::vector<wchar_t> mutableCmd(command.begin(), command.end());
    mutableCmd.push_back(L'\0');

    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(child.c_str(), mutableCmd.data(), nullptr, nullptr, FALSE, 0, nullptr, root.c_str(), &si, &pi)) {
        DWORD e = GetLastError();
        auto log = WriteLog(root, L"CreateProcessW fehlgeschlagen.\r\nWindows-Fehler: " + std::to_wstring(e) + L"\r\n" + WinError(e) + L"\r\nChild: " + child.wstring() + L"\r\n");
        MessageBoxW(nullptr, (L"Syntex Launcher konnte nicht gestartet werden.\n\nDiagnose: " + log.wstring()).c_str(), L"Syntex Launcher – Startfehler", MB_OK | MB_ICONERROR);
        return 93;
    }
    CloseHandle(pi.hThread);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(ciGui ? 25 : 12);
    while (std::chrono::steady_clock::now() < deadline) {
        DWORD wait = WaitForSingleObject(pi.hProcess, 100);
        if (wait == WAIT_OBJECT_0) {
            DWORD exitCode = 0;
            GetExitCodeProcess(pi.hProcess, &exitCode);
            CloseHandle(pi.hProcess);
            auto log = WriteLog(root, L"Launcher-Prozess wurde während der Startprüfung beendet.\r\nExitcode: " + std::to_wstring(exitCode) + L"\r\nChild: " + child.wstring() + L"\r\n");
            if (!ciGui)
                MessageBoxW(nullptr, (L"Syntex Launcher wurde beim Start unerwartet beendet.\n\nExitcode: " + std::to_wstring(exitCode) + L"\nDiagnose: " + log.wstring()).c_str(), L"Syntex Launcher – Startfehler", MB_OK | MB_ICONERROR);
            return exitCode == 0 ? 94 : static_cast<int>(exitCode & 0xFFu);
        }
        if (HasVisibleWindow(pi.dwProcessId)) {
            auto log = WriteLog(root, L"Startprüfung erfolgreich: sichtbares Launcher-Fenster erkannt.\r\nPID: " + std::to_wstring(pi.dwProcessId) + L"\r\n");
            if (ciGui) {
                TerminateProcess(pi.hProcess, 0);
                WaitForSingleObject(pi.hProcess, 5000);
            }
            CloseHandle(pi.hProcess);
            return log.empty() ? 95 : 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (ciGui) {
        TerminateProcess(pi.hProcess, 96);
        CloseHandle(pi.hProcess);
        WriteLog(root, L"CI GUI self-test fehlgeschlagen: innerhalb von 25 Sekunden kein sichtbares Fenster.\r\n");
        return 96;
    }

    WriteLog(root, L"Startprüfung: Launcher-Prozess läuft nach 12 Sekunden weiter.\r\nPID: " + std::to_wstring(pi.dwProcessId) + L"\r\n");
    CloseHandle(pi.hProcess);
    return 0;
}

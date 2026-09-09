#define UNICODE
#define _UNICODE
#include <windows.h>
#include <shlobj.h>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
static constexpr wchar_t kUninstallKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\SyntexLauncher";

static fs::path ExePath() {
    std::wstring b(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, b.data(), static_cast<DWORD>(b.size()));
    if (!n || n >= b.size()) return {};
    b.resize(n);
    return fs::path(b);
}

static fs::path KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &p)) || !p) return {};
    fs::path result(p);
    CoTaskMemFree(p);
    return result;
}

static std::wstring Quote(const std::wstring& s) { return L"\"" + s + L"\""; }

static void DeleteShortcuts() {
    std::error_code ec;
    fs::remove(KnownFolder(FOLDERID_Desktop) / L"Syntex Launcher.lnk", ec);
    ec.clear();
    fs::remove(KnownFolder(FOLDERID_StartMenu) / L"Programs" / L"Syntex Launcher.lnk", ec);
}

static void RemoveRegistry() {
    RegDeleteTreeW(HKEY_CURRENT_USER, kUninstallKey);
}

static bool SpawnCleanup(const fs::path& root, bool fullDelete) {
    wchar_t systemDir[MAX_PATH]{};
    if (!GetSystemDirectoryW(systemDir, MAX_PATH)) return false;
    fs::path cmd = fs::path(systemDir) / L"cmd.exe";
    std::wstring command;
    if (fullDelete) {
        command = Quote(cmd.wstring()) + L" /d /s /c \"timeout /t 2 /nobreak >nul & rmdir /s /q " + Quote(root.wstring()) + L"\"";
    } else {
        command = Quote(cmd.wstring()) + L" /d /s /c \"timeout /t 2 /nobreak >nul & rmdir /s /q " + Quote((root / L"app").wstring()) +
                  L" & del /f /q " + Quote((root / L"SyntexLauncher.exe").wstring()) + L"\"";
    }
    std::vector<wchar_t> mutableCmd(command.begin(), command.end());
    mutableCmd.push_back(L'\0');
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(cmd.c_str(), mutableCmd.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, root.c_str(), &si, &pi);
    if (ok) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
    return ok == TRUE;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    fs::path exe = ExePath();
    if (exe.empty()) return 10;
    fs::path root = exe.parent_path().parent_path();

    int answer = MessageBoxW(nullptr,
        L"Syntex Launcher wird deinstalliert.\n\nSollen auch persönliche Daten gelöscht werden?\n\n"
        L"Ja = alles löschen (Einstellungen, Accounts, Instanzen/Welten, Downloads und Runtimes)\n"
        L"Nein = Programm entfernen, persönliche Daten behalten\n"
        L"Abbrechen = nichts ändern",
        L"Syntex Launcher deinstallieren", MB_YESNOCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2);
    if (answer == IDCANCEL) return 0;

    const bool fullDelete = answer == IDYES;
    DeleteShortcuts();
    RemoveRegistry();

    if (!SpawnCleanup(root, fullDelete)) {
        MessageBoxW(nullptr,
            L"Die automatische Bereinigung konnte nicht gestartet werden. Bitte den Syntex-Launcher-Ordner nach dem Schließen manuell entfernen.",
            L"Syntex Launcher – Deinstallation", MB_OK | MB_ICONERROR);
        return 11;
    }

    if (fullDelete) {
        MessageBoxW(nullptr, L"Syntex Launcher und alle persönlichen Daten werden vollständig entfernt.", L"Syntex Launcher – Deinstallation", MB_OK | MB_ICONINFORMATION);
    } else {
        MessageBoxW(nullptr,
            (L"Syntex Launcher wird entfernt. Persönliche Daten bleiben erhalten unter:\n\n" + (root / L"storage").wstring()).c_str(),
            L"Syntex Launcher – Deinstallation", MB_OK | MB_ICONINFORMATION);
    }
    return 0;
}

#define UNICODE
#define _UNICODE
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <thread>
#include <algorithm>
#include <stdexcept>
#include "PayloadContract.h"

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")

namespace fs = std::filesystem;
static constexpr wchar_t kClassName[] = L"SyntexLauncherSetup0166RC";
static constexpr wchar_t kUninstallKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\SyntexLauncher";
static constexpr UINT WM_PROGRESS_UPDATE = WM_APP + 1;
static constexpr UINT WM_INSTALL_DONE = WM_APP + 2;

static HWND gWindow = nullptr;
static HWND gPathEdit = nullptr;
static HWND gStatus = nullptr;
static HWND gProgress = nullptr;
static HWND gPercent = nullptr;
static HWND gInstall = nullptr;
static HWND gBrowse = nullptr;

static std::wstring Quote(const std::wstring& value) { return L"\"" + value + L"\""; }

static fs::path ExePath() {
    std::wstring b(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, b.data(), static_cast<DWORD>(b.size()));
    if (!n || n >= b.size()) return {};
    b.resize(n);
    return fs::path(b);
}

static std::wstring Timestamp() {
    SYSTEMTIME st{}; GetLocalTime(&st);
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

static fs::path WriteSetupLog(const std::wstring& message) noexcept {
    try {
        fs::path setupDir = ExePath().parent_path();
        fs::path dir = setupDir / L"SyntexSetup-Log";
        std::error_code ec; fs::create_directories(dir, ec);
        if (ec) return {};
        fs::path path = dir / (L"SyntexSetup-Error-" + Timestamp() + L"-" + std::to_wstring(GetCurrentProcessId()) + L".log");
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f) return {};
        std::wstring body = L"Syntex Launcher Setup 0.16.6 RC1\r\nZeit: " + Timestamp() + L"\r\nSetup-Verzeichnis: " + setupDir.wstring() +
            L"\r\nProzess-ID: " + std::to_wstring(GetCurrentProcessId()) + L"\r\n\r\nFehler:\r\n" + message + L"\r\n";
        auto u8 = Utf8(body); f.write(u8.data(), static_cast<std::streamsize>(u8.size()));
        return path;
    } catch (...) { return {}; }
}

static LONG WINAPI TopLevelCrashFilter(EXCEPTION_POINTERS* info) {
    std::wstring msg = L"Unbehandelte native Ausnahme im Setup.";
    if (info && info->ExceptionRecord) {
        wchar_t b[32]{}; swprintf_s(b, L"0x%08X", info->ExceptionRecord->ExceptionCode);
        msg += L"\r\nException-Code: "; msg += b;
    }
    WriteSetupLog(msg);
    return EXCEPTION_EXECUTE_HANDLER;
}

static void PostProgress(int percent, const std::wstring& text) {
    if (!gWindow) return;
    auto* copy = new std::wstring(text);
    PostMessageW(gWindow, WM_PROGRESS_UPDATE, static_cast<WPARAM>(std::clamp(percent, 0, 100)), reinterpret_cast<LPARAM>(copy));
}

static fs::path KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &p)) || !p) return {};
    fs::path result(p); CoTaskMemFree(p); return result;
}

static std::wstring ExistingInstallPath() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kUninstallKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return {};
    DWORD type = 0, bytes = 0;
    if (RegQueryValueExW(key, L"InstallLocation", nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS || type != REG_SZ || bytes < sizeof(wchar_t)) {
        RegCloseKey(key); return {};
    }
    std::vector<wchar_t> value(bytes / sizeof(wchar_t) + 1, L'\0');
    if (RegQueryValueExW(key, L"InstallLocation", nullptr, &type, reinterpret_cast<BYTE*>(value.data()), &bytes) != ERROR_SUCCESS) {
        RegCloseKey(key); return {};
    }
    RegCloseKey(key);
    return value.data();
}

static std::wstring DefaultInstallPath() {
    auto registered = ExistingInstallPath();
    if (!registered.empty()) return registered;
    fs::path local = KnownFolder(FOLDERID_LocalAppData);
    if (local.empty()) return L"C:\\SyntexLauncher";
    return (local / L"Syntex Launcher").wstring();
}

static bool ValidateWritableRoot(const fs::path& root, std::wstring& error) {
    std::error_code ec; fs::create_directories(root, ec);
    if (ec) { error = L"Installationsordner konnte nicht erstellt werden."; return false; }
    fs::path a = root / L".syntex-write-test.tmp";
    fs::path b = root / L".syntex-write-test-renamed.tmp";
    {
        std::ofstream f(a, std::ios::binary | std::ios::trunc);
        if (!f) { error = L"Installationsordner ist nicht beschreibbar."; return false; }
        f << "syntex";
    }
    fs::rename(a, b, ec);
    if (ec) { fs::remove(a); error = L"Installationsordner erlaubt kein Umbenennen von Dateien."; return false; }
    fs::remove(b, ec);
    if (ec) { error = L"Installationsordner erlaubt kein Löschen von Dateien."; return false; }
    return true;
}

static bool HttpDownload(const std::wstring& url, const fs::path& target, unsigned long long expectedSize, std::wstring& error, bool reportProgress) {
    URL_COMPONENTS parts{}; parts.dwStructSize = sizeof(parts);
    wchar_t host[256]{}; wchar_t path[4096]{};
    parts.lpszHostName = host; parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
    parts.lpszUrlPath = path; parts.dwUrlPathLength = static_cast<DWORD>(std::size(path));
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS) {
        error = L"Ungültige oder unsichere Download-Adresse."; return false;
    }
    HINTERNET session = WinHttpOpen(L"SyntexLauncherSetup/0.16.6-RC1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { error = L"WinHTTP konnte nicht initialisiert werden."; return false; }
    WinHttpSetTimeouts(session, 15000, 15000, 60000, 60000);
    HINTERNET connect = WinHttpConnect(session, host, parts.nPort, 0);
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", path, nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    if (!connect || !request || !WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(request, nullptr)) {
        if (request) WinHttpCloseHandle(request); if (connect) WinHttpCloseHandle(connect); WinHttpCloseHandle(session);
        error = L"Launcher-Download fehlgeschlagen."; return false;
    }
    DWORD status = 0, statusSize = sizeof(status);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
        &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
    if (status < 200 || status >= 300) {
        error = L"Launcher-Download: HTTP " + std::to_wstring(status) + L".";
        WinHttpCloseHandle(request); WinHttpCloseHandle(connect); WinHttpCloseHandle(session); return false;
    }
    std::ofstream f(target, std::ios::binary | std::ios::trunc);
    if (!f) { error = L"Temporäre Download-Datei konnte nicht angelegt werden."; WinHttpCloseHandle(request); WinHttpCloseHandle(connect); WinHttpCloseHandle(session); return false; }
    unsigned long long total = 0;
    DWORD available = 0;
    while (WinHttpQueryDataAvailable(request, &available) && available > 0) {
        std::vector<char> buffer(available); DWORD read = 0;
        if (!WinHttpReadData(request, buffer.data(), available, &read)) { error = L"Launcher-Download wurde unterbrochen."; f.close(); WinHttpCloseHandle(request); WinHttpCloseHandle(connect); WinHttpCloseHandle(session); return false; }
        f.write(buffer.data(), read); total += read;
        if (reportProgress && expectedSize > 0) {
            int raw = static_cast<int>(std::min<unsigned long long>(100, total * 100ULL / expectedSize));
            PostProgress(12 + raw * 58 / 100, L"Launcher wird heruntergeladen … " + std::to_wstring(raw) + L" %");
        }
    }
    f.close(); WinHttpCloseHandle(request); WinHttpCloseHandle(connect); WinHttpCloseHandle(session);
    if (expectedSize && total != expectedSize) { error = L"Die heruntergeladene Dateigröße stimmt nicht mit dem geprüften Paket überein."; return false; }
    return true;
}

static bool Sha256File(const fs::path& path, std::wstring& hex, std::wstring& error) {
    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectSize = 0, cb = 0, hashSize = 0;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &cb, 0) < 0 ||
        BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashSize), sizeof(hashSize), &cb, 0) < 0) {
        if (alg) BCryptCloseAlgorithmProvider(alg, 0); error = L"SHA-256 konnte nicht initialisiert werden."; return false;
    }
    std::vector<UCHAR> object(objectSize), digest(hashSize);
    if (BCryptCreateHash(alg, &hash, object.data(), objectSize, nullptr, 0, 0) < 0) { BCryptCloseAlgorithmProvider(alg, 0); error = L"SHA-256 Hash konnte nicht erstellt werden."; return false; }
    std::ifstream f(path, std::ios::binary);
    if (!f) { BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(alg, 0); error = L"Download-Datei konnte nicht geprüft werden."; return false; }
    std::vector<UCHAR> buffer(1024 * 1024);
    while (f) {
        f.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        ULONG n = static_cast<ULONG>(f.gcount());
        if (n && BCryptHashData(hash, buffer.data(), n, 0) < 0) { BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(alg, 0); error = L"SHA-256 Prüfung fehlgeschlagen."; return false; }
    }
    if (BCryptFinishHash(hash, digest.data(), hashSize, 0) < 0) { BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(alg, 0); error = L"SHA-256 Prüfung konnte nicht abgeschlossen werden."; return false; }
    static constexpr wchar_t digits[] = L"0123456789abcdef";
    hex.clear();
    for (auto b : digest) { hex.push_back(digits[b >> 4]); hex.push_back(digits[b & 15]); }
    BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(alg, 0);
    return true;
}

static bool RunProcess(const std::wstring& command, const fs::path& cwd, DWORD& exitCode, std::wstring& error, DWORD timeout = INFINITE) {
    std::vector<wchar_t> mutableCmd(command.begin(), command.end()); mutableCmd.push_back(L'\0');
    STARTUPINFOW si{}; si.cb = sizeof(si); PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, cwd.empty() ? nullptr : cwd.c_str(), &si, &pi)) {
        error = L"Windows konnte ein benötigtes Hilfsprogramm nicht starten. Fehler " + std::to_wstring(GetLastError()) + L"."; return false;
    }
    CloseHandle(pi.hThread);
    DWORD wait = WaitForSingleObject(pi.hProcess, timeout);
    if (wait == WAIT_TIMEOUT) { TerminateProcess(pi.hProcess, 124); CloseHandle(pi.hProcess); error = L"Ein Installationsschritt hat das Zeitlimit überschritten."; return false; }
    if (wait != WAIT_OBJECT_0) { CloseHandle(pi.hProcess); error = L"Ein Installationsschritt konnte nicht überwacht werden."; return false; }
    GetExitCodeProcess(pi.hProcess, &exitCode); CloseHandle(pi.hProcess); return true;
}

static bool ExtractZip(const fs::path& zip, const fs::path& target, std::wstring& error) {
    wchar_t systemDir[MAX_PATH]{}; if (!GetSystemDirectoryW(systemDir, MAX_PATH)) { error = L"Windows-Systemordner konnte nicht ermittelt werden."; return false; }
    fs::path tar = fs::path(systemDir) / L"tar.exe";
    if (!fs::exists(tar)) { error = L"Windows-Systemwerkzeug tar.exe ist nicht verfügbar."; return false; }
    std::error_code ec; fs::create_directories(target, ec); if (ec) { error = L"Temporärer Installationsbereich konnte nicht erstellt werden."; return false; }
    DWORD code = 0;
    if (!RunProcess(Quote(tar.wstring()) + L" -xf " + Quote(zip.wstring()) + L" -C " + Quote(target.wstring()), target, code, error)) return false;
    if (code != 0) { error = L"Launcher-Paket konnte nicht entpackt werden (tar Exitcode " + std::to_wstring(code) + L")."; return false; }
    return true;
}

static bool ValidateStage(const fs::path& stage, std::wstring& error) {
    const fs::path required[] = { stage / L"SyntexLauncher.exe", stage / L"app" / L"SyntexLauncher.App.exe", stage / L"app" / L"SyntexUninstall.exe" };
    for (const auto& file : required) if (!fs::exists(file)) { error = L"Installationspaket ist unvollständig: " + file.filename().wstring() + L" fehlt."; return false; }
    return true;
}

static bool MoveMerge(const fs::path& source, const fs::path& destination, std::wstring& error) {
    if (!fs::exists(source)) return true;
    std::error_code ec; fs::create_directories(destination, ec); if (ec) { error = L"Bestehende Nutzerdaten konnten nicht vorbereitet werden: " + source.wstring(); return false; }
    for (const auto& entry : fs::directory_iterator(source, ec)) {
        if (ec) { error = L"Bestehende Nutzerdaten konnten nicht gelesen werden: " + source.wstring(); return false; }
        fs::path target = destination / entry.path().filename();
        if (entry.is_directory()) {
            if (!MoveMerge(entry.path(), target, error)) return false;
        } else {
            if (!fs::exists(target)) {
                fs::rename(entry.path(), target, ec);
                if (ec) { ec.clear(); fs::copy_file(entry.path(), target, fs::copy_options::overwrite_existing, ec); if (!ec) fs::remove(entry.path(), ec); }
                if (ec) { error = L"Nutzerdatendatei konnte nicht übernommen werden: " + entry.path().wstring(); return false; }
            }
        }
    }
    fs::remove_all(source, ec);
    return true;
}

static bool PrepareRepairRoot(const fs::path& root, bool& repair, std::wstring& error) {
    std::error_code ec;
    repair = false;
    for (const auto& marker : { root / L"SyntexLauncher.exe", root / L"app", root / L"data", root / L"instances", root / L"runtimes" }) {
        if (fs::exists(marker, ec)) { repair = true; break; }
    }
    fs::path storage = root / L"storage"; fs::create_directories(storage, ec);
    if (ec) { error = L"Speicherordner konnte nicht angelegt werden."; return false; }

    const wchar_t* legacy[] = { L"launcher", L"data", L"instances", L"runtimes", L"cache", L"downloads", L"logs", L"creator", L"diagnostics", L"temp", L"updates", L"fehleranalysen" };
    for (const auto* name : legacy) {
        fs::path src = root / name;
        if (fs::exists(src) && !MoveMerge(src, storage / name, error)) return false;
    }

    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (ec) { error = L"Vorhandene Installation konnte nicht gelesen werden."; return false; }
        if (_wcsicmp(entry.path().filename().c_str(), L"storage") == 0) continue;
        fs::remove_all(entry.path(), ec);
        if (ec) { error = L"Vorhandene Programmdateien sind noch in Benutzung und konnten nicht ersetzt werden: " + entry.path().filename().wstring() + L". Bitte Syntex Launcher vollständig schließen und Setup erneut starten."; return false; }
    }
    return true;
}

static bool CopyStage(const fs::path& stage, const fs::path& root, std::wstring& error) {
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(stage, ec)) {
        if (ec) { error = L"Temporäres Installationspaket konnte nicht gelesen werden."; return false; }
        fs::copy(entry.path(), root / entry.path().filename(), fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        if (ec) { error = L"Neue Programmdateien konnten nicht vollständig installiert werden: " + entry.path().filename().wstring(); return false; }
    }
    return true;
}

static bool CreateShortcut(const fs::path& target, const fs::path& shortcut, std::wstring& error) {
    HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); bool uninit = SUCCEEDED(init);
    IShellLinkW* link = nullptr; HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link));
    if (FAILED(hr) || !link) { if (uninit) CoUninitialize(); error = L"Verknüpfung konnte nicht erstellt werden."; return false; }
    link->SetPath(target.c_str()); link->SetWorkingDirectory(target.parent_path().c_str()); link->SetDescription(L"Syntex Launcher");
    IPersistFile* persist = nullptr; hr = link->QueryInterface(IID_PPV_ARGS(&persist));
    if (SUCCEEDED(hr) && persist) {
        std::error_code ec; fs::create_directories(shortcut.parent_path(), ec);
        hr = persist->Save(shortcut.c_str(), TRUE); persist->Release();
    }
    link->Release(); if (uninit) CoUninitialize();
    if (FAILED(hr)) { error = L"Verknüpfung konnte nicht gespeichert werden."; return false; }
    return true;
}

static bool SetRegString(HKEY key, const wchar_t* name, const std::wstring& value) {
    return RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

static bool RegisterUninstaller(const fs::path& root, std::wstring& error) {
    fs::path uninstaller = root / L"app" / L"SyntexUninstall.exe";
    HKEY key = nullptr; DWORD disp = 0;
    LONG rc = RegCreateKeyExW(HKEY_CURRENT_USER, kUninstallKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, &disp);
    if (rc != ERROR_SUCCESS || !key) { error = L"Windows-Deinstallation konnte nicht registriert werden (Fehler " + std::to_wstring(rc) + L")."; return false; }
    DWORD noModify = 1, noRepair = 1; DWORD estimatedKb = static_cast<DWORD>(std::min<unsigned long long>(PAYLOAD_SIZE / 1024ULL, 0xFFFFFFFFULL));
    SYSTEMTIME st{}; GetLocalTime(&st); wchar_t date[16]{}; swprintf_s(date, L"%04u%02u%02u", st.wYear, st.wMonth, st.wDay);
    bool ok = SetRegString(key, L"DisplayName", L"Syntex Launcher") && SetRegString(key, L"DisplayVersion", L"0.16.6") &&
        SetRegString(key, L"Publisher", L"Syntex Launcher Project") && SetRegString(key, L"InstallLocation", root.wstring()) &&
        SetRegString(key, L"DisplayIcon", (root / L"SyntexLauncher.exe").wstring() + L",0") && SetRegString(key, L"UninstallString", Quote(uninstaller.wstring())) &&
        SetRegString(key, L"InstallDate", date) &&
        RegSetValueExW(key, L"NoModify", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&noModify), sizeof(noModify)) == ERROR_SUCCESS &&
        RegSetValueExW(key, L"NoRepair", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&noRepair), sizeof(noRepair)) == ERROR_SUCCESS &&
        RegSetValueExW(key, L"EstimatedSize", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&estimatedKb), sizeof(estimatedKb)) == ERROR_SUCCESS;
    RegCloseKey(key);
    if (!ok) { error = L"Windows-Deinstallation konnte nicht vollständig registriert werden."; return false; }
    return true;
}

static bool LaunchVerify(const fs::path& root, bool ciMode, std::wstring& error) {
    fs::path host = root / L"SyntexLauncher.exe";
    DWORD code = 0;
    std::wstring command = Quote(host.wstring()) + (ciMode ? L" --ci-gui-self-test" : L"");
    if (!RunProcess(command, root, code, error, 35000)) return false;
    if (code != 0) { error = L"Syntex Launcher konnte nach der Installation nicht erfolgreich gestartet werden. Startwächter-Exitcode: " + std::to_wstring(code) + L". Prüfe storage\\logs\\launcher-startup."; return false; }
    return true;
}

static bool InstallCore(const fs::path& root, bool ciMode, bool reportProgress, bool& wasRepair, std::wstring& error) {
    fs::path temp = fs::temp_directory_path() / (L"SyntexLauncher-0166RC-" + std::to_wstring(GetCurrentProcessId()) + L".zip");
    fs::path stage = fs::temp_directory_path() / (L"SyntexLauncher-0166RC-stage-" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec; fs::remove(temp, ec); fs::remove_all(stage, ec);
    auto progress = [&](int p, const std::wstring& s) { if (reportProgress) PostProgress(p, s); };

    progress(3, L"Installationsordner wird geprüft …");
    if (!ValidateWritableRoot(root, error)) return false;
    progress(8, L"Geprüftes Reparatur-/Installationspaket wird vorbereitet …");
    if (!HttpDownload(PAYLOAD_URL, temp, PAYLOAD_SIZE, error, reportProgress)) return false;
    progress(73, L"Download wird mit SHA-256 geprüft …");
    std::wstring actual; if (!Sha256File(temp, actual, error)) return false;
    if (_wcsicmp(actual.c_str(), PAYLOAD_SHA256) != 0) { error = L"Sicherheitsprüfung fehlgeschlagen: SHA-256 stimmt nicht mit dem veröffentlichten Paket überein."; fs::remove(temp, ec); return false; }
    progress(80, L"Neue Programmdateien werden vorab entpackt und geprüft …");
    if (!ExtractZip(temp, stage, error) || !ValidateStage(stage, error)) { fs::remove(temp, ec); fs::remove_all(stage, ec); return false; }

    progress(86, L"Vorhandene Installation wird erkannt und Nutzerdaten werden gesichert …");
    if (!PrepareRepairRoot(root, wasRepair, error)) { fs::remove(temp, ec); fs::remove_all(stage, ec); return false; }
    progress(90, wasRepair ? L"Fehlerhafte/veraltete Programmdateien werden ersetzt …" : L"Syntex Launcher wird installiert …");
    if (!CopyStage(stage, root, error)) { fs::remove(temp, ec); fs::remove_all(stage, ec); return false; }
    fs::remove(temp, ec); fs::remove_all(stage, ec);

    if (!ciMode) {
        progress(94, L"Verknüpfungen und Windows-Deinstallation werden eingerichtet …");
        std::wstring shortcutError;
        if (!CreateShortcut(root / L"SyntexLauncher.exe", KnownFolder(FOLDERID_Desktop) / L"Syntex Launcher.lnk", shortcutError) ||
            !CreateShortcut(root / L"SyntexLauncher.exe", KnownFolder(FOLDERID_StartMenu) / L"Programs" / L"Syntex Launcher.lnk", shortcutError)) {
            error = shortcutError; return false;
        }
        if (!RegisterUninstaller(root, error)) return false;
    }

    progress(98, L"Syntex Launcher wird gestartet und geprüft …");
    if (!LaunchVerify(root, ciMode, error)) return false;
    progress(100, wasRepair ? L"Reparatur/Aktualisierung abgeschlossen." : L"Installation abgeschlossen.");
    return true;
}

static bool RepairSelfTest() {
    std::error_code ec;
    fs::path root = fs::temp_directory_path() / (L"SyntexRepairSelfTest-" + std::to_wstring(GetCurrentProcessId()));
    fs::path stage = root; stage += L"-stage";
    fs::remove_all(root, ec); fs::remove_all(stage, ec);
    fs::create_directories(root / L"data" / L"settings", ec);
    fs::create_directories(root / L"instances" / L"world-a", ec);
    fs::create_directories(root / L"tools", ec);
    { std::ofstream(root / L"SyntexLauncher.exe") << "old"; std::ofstream(root / L"Old.Managed.dll") << "old"; std::ofstream(root / L"data" / L"settings" / L"settings.json") << "keep"; std::ofstream(root / L"instances" / L"world-a" / L"level.dat") << "keep"; }
    fs::create_directories(stage / L"app", ec);
    { std::ofstream(stage / L"SyntexLauncher.exe") << "new"; std::ofstream(stage / L"app" / L"SyntexLauncher.App.exe") << "new"; std::ofstream(stage / L"app" / L"SyntexUninstall.exe") << "new"; }
    std::wstring error; bool repair = false;
    bool ok = ValidateStage(stage, error) && PrepareRepairRoot(root, repair, error) && repair && CopyStage(stage, root, error) &&
        !fs::exists(root / L"Old.Managed.dll") && !fs::exists(root / L"tools") && fs::exists(root / L"SyntexLauncher.exe") &&
        fs::exists(root / L"app" / L"SyntexLauncher.App.exe") && fs::exists(root / L"storage" / L"data" / L"settings" / L"settings.json") &&
        fs::exists(root / L"storage" / L"instances" / L"world-a" / L"level.dat");
    fs::remove_all(root, ec); fs::remove_all(stage, ec); return ok;
}

static std::wstring BrowseForFolder(HWND owner) {
    BROWSEINFOW bi{}; bi.hwndOwner = owner; bi.lpszTitle = L"Installationsordner für Syntex Launcher auswählen"; bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi); if (!pidl) return {};
    wchar_t path[MAX_PATH]{}; std::wstring result; if (SHGetPathFromIDListW(pidl, path)) result = path; CoTaskMemFree(pidl); return result;
}

static void Worker(std::wstring rootText) {
    bool repair = false; std::wstring error;
    bool ok = InstallCore(fs::path(rootText), false, true, repair, error);
    if (ok) {
        PostMessageW(gWindow, WM_INSTALL_DONE, repair ? 2 : 1, 0);
    } else {
        auto log = WriteSetupLog(error);
        if (!log.empty()) error += L"\r\n\r\nFehlerprotokoll:\r\n" + log.wstring();
        auto* copy = new std::wstring(error.empty() ? L"Unbekannter Installationsfehler." : error);
        PostMessageW(gWindow, WM_INSTALL_DONE, 0, reinterpret_cast<LPARAM>(copy));
    }
}

static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
            CreateWindowW(L"STATIC", L"Syntex Launcher Setup 0.16.6", WS_CHILD | WS_VISIBLE, 24, 20, 520, 28, hwnd, nullptr, nullptr, nullptr);
            CreateWindowW(L"STATIC", L"Installiert, aktualisiert oder repariert eine vorhandene Syntex-Installation.", WS_CHILD | WS_VISIBLE, 24, 52, 540, 20, hwnd, nullptr, nullptr, nullptr);
            CreateWindowW(L"STATIC", L"Installationsordner:", WS_CHILD | WS_VISIBLE, 24, 88, 160, 20, hwnd, nullptr, nullptr, nullptr);
            std::wstring initial = DefaultInstallPath();
            gPathEdit = CreateWindowW(L"EDIT", initial.c_str(), WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 24, 110, 430, 26, hwnd, reinterpret_cast<HMENU>(1001), nullptr, nullptr);
            gBrowse = CreateWindowW(L"BUTTON", L"Durchsuchen…", WS_CHILD | WS_VISIBLE, 462, 109, 100, 28, hwnd, reinterpret_cast<HMENU>(1002), nullptr, nullptr);
            bool existing = fs::exists(fs::path(initial) / L"SyntexLauncher.exe") || fs::exists(fs::path(initial) / L"app") || fs::exists(fs::path(initial) / L"data");
            gStatus = CreateWindowW(L"STATIC", existing ? L"Vorhandene Installation erkannt – sie wird repariert/aktualisiert." : L"Bereit.", WS_CHILD | WS_VISIBLE, 24, 151, 538, 30, hwnd, nullptr, nullptr, nullptr);
            gProgress = CreateWindowExW(0, PROGRESS_CLASSW, nullptr, WS_CHILD | WS_VISIBLE, 24, 187, 470, 20, hwnd, reinterpret_cast<HMENU>(1004), nullptr, nullptr);
            gPercent = CreateWindowW(L"STATIC", L"0 %", WS_CHILD | WS_VISIBLE | SS_RIGHT, 502, 186, 60, 22, hwnd, nullptr, nullptr, nullptr);
            gInstall = CreateWindowW(L"BUTTON", existing ? L"Reparieren" : L"Installieren", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 350, 229, 100, 32, hwnd, reinterpret_cast<HMENU>(1003), nullptr, nullptr);
            HWND cancel = CreateWindowW(L"BUTTON", L"Abbrechen", WS_CHILD | WS_VISIBLE, 462, 229, 100, 32, hwnd, reinterpret_cast<HMENU>(IDCANCEL), nullptr, nullptr);
            if (gProgress) {
                SendMessageW(gProgress, PBM_SETRANGE32, 0, 100); SendMessageW(gProgress, PBM_SETPOS, 0, 0);
                SetWindowTheme(gProgress, L"", L"");
                SendMessageW(gProgress, PBM_SETBARCOLOR, 0, RGB(0, 120, 215));
                SendMessageW(gProgress, PBM_SETBKCOLOR, 0, RGB(232, 232, 232));
            }
            for (HWND c = GetWindow(hwnd, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == 1002) { auto p = BrowseForFolder(hwnd); if (!p.empty()) SetWindowTextW(gPathEdit, p.c_str()); return 0; }
            if (id == 1003) {
                wchar_t b[4096]{}; GetWindowTextW(gPathEdit, b, static_cast<int>(std::size(b)));
                if (!wcslen(b)) { MessageBoxW(hwnd, L"Bitte einen Installationsordner auswählen.", L"Syntex Launcher Setup", MB_OK | MB_ICONWARNING); return 0; }
                EnableWindow(gInstall, FALSE); EnableWindow(gBrowse, FALSE); EnableWindow(gPathEdit, FALSE);
                SendMessageW(gProgress, PBM_SETPOS, 0, 0); SetWindowTextW(gPercent, L"0 %");
                std::thread(Worker, std::wstring(b)).detach(); return 0;
            }
            if (id == IDCANCEL) { DestroyWindow(hwnd); return 0; }
            break;
        }
        case WM_PROGRESS_UPDATE: {
            int p = static_cast<int>(wParam); auto* text = reinterpret_cast<std::wstring*>(lParam);
            if (text) { SetWindowTextW(gStatus, text->c_str()); delete text; }
            SendMessageW(gProgress, PBM_SETPOS, p, 0); std::wstring pct = std::to_wstring(p) + L" %"; SetWindowTextW(gPercent, pct.c_str()); return 0;
        }
        case WM_INSTALL_DONE: {
            if (wParam == 1 || wParam == 2) {
                SendMessageW(gProgress, PBM_SETPOS, 100, 0); SetWindowTextW(gPercent, L"100 %");
                bool repair = wParam == 2;
                SetWindowTextW(gStatus, repair ? L"Reparatur/Aktualisierung abgeschlossen." : L"Installation abgeschlossen.");
                MessageBoxW(hwnd, repair ? L"Die vorhandene Syntex-Installation wurde repariert/aktualisiert und der Launcher-Start wurde geprüft." : L"Syntex Launcher wurde installiert und der Launcher-Start wurde geprüft.",
                    L"Syntex Launcher Setup", MB_OK | MB_ICONINFORMATION);
                DestroyWindow(hwnd);
            } else {
                auto* text = reinterpret_cast<std::wstring*>(lParam); std::wstring message = text ? *text : L"Unbekannter Installationsfehler."; delete text;
                SetWindowTextW(gStatus, L"Installation/Reparatur fehlgeschlagen."); EnableWindow(gInstall, TRUE); EnableWindow(gBrowse, TRUE); EnableWindow(gPathEdit, TRUE);
                MessageBoxW(hwnd, message.c_str(), L"Syntex Launcher Setup – Fehler", MB_OK | MB_ICONERROR);
            }
            return 0;
        }
        case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    SetUnhandledExceptionFilter(TopLevelCrashFilter);
    int argc = 0; LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc == 2 && _wcsicmp(argv[1], L"--self-test") == 0) { LocalFree(argv); return (wcslen(PAYLOAD_SHA256) == 64 && PAYLOAD_SIZE > 0 && wcsncmp(PAYLOAD_URL, L"https://", 8) == 0) ? 0 : 2; }
    if (argv && argc == 2 && _wcsicmp(argv[1], L"--log-self-test") == 0) { auto p = WriteSetupLog(L"SyntexSetup-Log CI self-test"); LocalFree(argv); return p.empty() ? 3 : 0; }
    if (argv && argc == 2 && _wcsicmp(argv[1], L"--repair-self-test") == 0) { bool ok = RepairSelfTest(); LocalFree(argv); return ok ? 0 : 4; }
    if (argv && argc == 3 && _wcsicmp(argv[1], L"--ci-install") == 0) {
        fs::path target(argv[2]); LocalFree(argv); bool repair = false; std::wstring error;
        bool ok = InstallCore(target, true, false, repair, error);
        if (!ok) { WriteSetupLog(L"CI install E2E: " + error); return 5; }
        std::error_code ec; fs::remove_all(target, ec); return 0;
    }
    if (argv) LocalFree(argv);

    INITCOMMONCONTROLSEX cc{}; cc.dwSize = sizeof(cc); cc.dwICC = ICC_PROGRESS_CLASS; InitCommonControlsEx(&cc);
    WNDCLASSEXW wc{}; wc.cbSize = sizeof(wc); wc.lpfnWndProc = WindowProc; wc.hInstance = instance; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1); wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc)) return 10;
    gWindow = CreateWindowExW(0, kClassName, L"Syntex Launcher Setup 0.16.6", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 600, 315, nullptr, nullptr, instance, nullptr);
    if (!gWindow) return 11;
    ShowWindow(gWindow, show); UpdateWindow(gWindow);
    MSG msg{}; while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return static_cast<int>(msg.wParam);
}

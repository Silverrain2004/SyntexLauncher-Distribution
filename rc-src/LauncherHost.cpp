#define UNICODE
#define _UNICODE
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <algorithm>
#include <system_error>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")

namespace fs = std::filesystem;

static constexpr wchar_t kCurrentVersion[] = L"0.16.7";
static constexpr wchar_t kManifestUrl[] = L"https://raw.githubusercontent.com/Silverrain2004/SyntexLauncher-Distribution/main/distribution.json";

static fs::path ExePath() {
    std::wstring buffer(32768, L'\0');
    const DWORD len = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
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
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n, nullptr, nullptr);
    return out;
}

static std::wstring Wide(const std::string& text) {
    if (text.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), out.data(), n);
    return out;
}

static fs::path WriteLog(const fs::path& root, const std::wstring& text) {
    std::error_code ec;
    const fs::path dir = root / L"storage" / L"logs" / L"launcher-startup";
    fs::create_directories(dir, ec);
    if (ec) return {};
    const fs::path path = dir / (L"SyntexLauncher-Start-" + Timestamp() + L"-" + std::to_wstring(GetCurrentProcessId()) + L".log");
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return {};
    const auto u8 = Utf8(text);
    f.write(u8.data(), static_cast<std::streamsize>(u8.size()));
    return path;
}

static void AppendUpdateLog(const fs::path& root, const std::wstring& text) noexcept {
    try {
        std::error_code ec;
        const fs::path dir = root / L"storage" / L"logs" / L"launcher-update";
        fs::create_directories(dir, ec);
        if (ec) return;
        const fs::path path = dir / L"update-check.log";
        std::ofstream f(path, std::ios::binary | std::ios::app);
        if (!f) return;
        const auto u8 = Utf8(L"[" + Timestamp() + L"] " + text + L"\r\n");
        f.write(u8.data(), static_cast<std::streamsize>(u8.size()));
    } catch (...) {
    }
}

static std::wstring WinError(DWORD code) {
    wchar_t* msg = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<LPWSTR>(&msg), 0, nullptr);
    const std::wstring result = n && msg ? std::wstring(msg, n) : L"Unbekannter Windows-Fehler";
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

static bool HasArg(const std::vector<std::wstring>& args, const wchar_t* value) {
    for (const auto& arg : args) {
        if (_wcsicmp(arg.c_str(), value) == 0) return true;
    }
    return false;
}

static bool CrackHttpsUrl(const std::wstring& url, URL_COMPONENTS& parts, std::vector<wchar_t>& host, std::vector<wchar_t>& path) {
    host.assign(512, L'\0');
    path.assign(8192, L'\0');
    parts = {};
    parts.dwStructSize = sizeof(parts);
    parts.lpszHostName = host.data();
    parts.dwHostNameLength = static_cast<DWORD>(host.size());
    parts.lpszUrlPath = path.data();
    parts.dwUrlPathLength = static_cast<DWORD>(path.size());
    return WinHttpCrackUrl(url.c_str(), 0, 0, &parts) && parts.nScheme == INTERNET_SCHEME_HTTPS;
}

static bool HttpGetBytes(const std::wstring& url, std::vector<unsigned char>& bytes, std::wstring& error, DWORD receiveTimeoutMs, size_t maxBytes = 0) {
    URL_COMPONENTS parts{};
    std::vector<wchar_t> host, path;
    if (!CrackHttpsUrl(url, parts, host, path)) {
        error = L"Unsichere oder ungültige HTTPS-Adresse.";
        return false;
    }

    HINTERNET session = WinHttpOpen(L"SyntexLauncher/0.16.7", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        error = L"WinHTTP konnte nicht initialisiert werden.";
        return false;
    }
    WinHttpSetTimeouts(session, 3000, 3000, 5000, static_cast<int>(receiveTimeoutMs));
    HINTERNET connect = WinHttpConnect(session, host.data(), parts.nPort, 0);
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", path.data(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    if (!connect || !request || !WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, nullptr)) {
        if (request) WinHttpCloseHandle(request);
        if (connect) WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        error = L"HTTPS-Abfrage fehlgeschlagen.";
        return false;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
        &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
    if (status < 200 || status >= 300) {
        error = L"HTTPS-Abfrage antwortete mit HTTP " + std::to_wstring(status) + L".";
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }

    bytes.clear();
    DWORD available = 0;
    while (WinHttpQueryDataAvailable(request, &available) && available > 0) {
        if (maxBytes > 0 && bytes.size() + available > maxBytes) {
            error = L"Serverantwort überschreitet die erlaubte Größe.";
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connect);
            WinHttpCloseHandle(session);
            return false;
        }
        const size_t oldSize = bytes.size();
        bytes.resize(oldSize + available);
        DWORD read = 0;
        if (!WinHttpReadData(request, bytes.data() + oldSize, available, &read)) {
            error = L"HTTPS-Download wurde unterbrochen.";
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connect);
            WinHttpCloseHandle(session);
            return false;
        }
        bytes.resize(oldSize + read);
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return true;
}

static bool HttpDownloadFile(const std::wstring& url, const fs::path& target, unsigned long long expectedSize, std::wstring& error) {
    URL_COMPONENTS parts{};
    std::vector<wchar_t> host, path;
    if (!CrackHttpsUrl(url, parts, host, path)) {
        error = L"Unsichere oder ungültige Update-Adresse.";
        return false;
    }

    HINTERNET session = WinHttpOpen(L"SyntexLauncher/0.16.7", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        error = L"WinHTTP konnte nicht initialisiert werden.";
        return false;
    }
    WinHttpSetTimeouts(session, 5000, 5000, 15000, 120000);
    HINTERNET connect = WinHttpConnect(session, host.data(), parts.nPort, 0);
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", path.data(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    if (!connect || !request || !WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, nullptr)) {
        if (request) WinHttpCloseHandle(request);
        if (connect) WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        error = L"Update-Download fehlgeschlagen.";
        return false;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
        &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
    if (status < 200 || status >= 300) {
        error = L"Update-Download antwortete mit HTTP " + std::to_wstring(status) + L".";
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }

    std::ofstream file(target, std::ios::binary | std::ios::trunc);
    if (!file) {
        error = L"Temporäre Update-Datei konnte nicht erstellt werden.";
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }

    unsigned long long total = 0;
    DWORD available = 0;
    while (WinHttpQueryDataAvailable(request, &available) && available > 0) {
        std::vector<unsigned char> buffer(available);
        DWORD read = 0;
        if (!WinHttpReadData(request, buffer.data(), available, &read)) {
            error = L"Update-Download wurde unterbrochen.";
            file.close();
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connect);
            WinHttpCloseHandle(session);
            return false;
        }
        file.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(read));
        total += read;
        if (expectedSize > 0 && total > expectedSize) {
            error = L"Update-Download ist größer als im Manifest angegeben.";
            file.close();
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connect);
            WinHttpCloseHandle(session);
            return false;
        }
    }

    file.close();
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    if (expectedSize > 0 && total != expectedSize) {
        error = L"Update-Dateigröße stimmt nicht mit dem Manifest überein.";
        return false;
    }
    return true;
}

static bool Sha256File(const fs::path& path, std::wstring& hex, std::wstring& error) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectSize = 0, cb = 0, hashSize = 0;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &cb, 0) < 0 ||
        BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashSize), sizeof(hashSize), &cb, 0) < 0) {
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
        error = L"SHA-256 konnte nicht initialisiert werden.";
        return false;
    }
    std::vector<UCHAR> object(objectSize), digest(hashSize);
    if (BCryptCreateHash(alg, &hash, object.data(), objectSize, nullptr, 0, 0) < 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        error = L"SHA-256 Hash konnte nicht erstellt werden.";
        return false;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(alg, 0);
        error = L"Update-Datei konnte nicht für SHA-256 geöffnet werden.";
        return false;
    }
    std::vector<UCHAR> buffer(1024 * 1024);
    while (file) {
        file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        const ULONG n = static_cast<ULONG>(file.gcount());
        if (n > 0 && BCryptHashData(hash, buffer.data(), n, 0) < 0) {
            BCryptDestroyHash(hash);
            BCryptCloseAlgorithmProvider(alg, 0);
            error = L"SHA-256 Prüfung fehlgeschlagen.";
            return false;
        }
    }
    if (BCryptFinishHash(hash, digest.data(), hashSize, 0) < 0) {
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(alg, 0);
        error = L"SHA-256 Prüfung konnte nicht abgeschlossen werden.";
        return false;
    }
    static constexpr wchar_t digits[] = L"0123456789abcdef";
    hex.clear();
    for (const auto b : digest) {
        hex.push_back(digits[b >> 4]);
        hex.push_back(digits[b & 15]);
    }
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    return true;
}

static std::string JsonStringAfter(const std::string& json, const std::string& key, size_t start = 0) {
    const std::string needle = "\"" + key + "\"";
    const size_t keyPos = json.find(needle, start);
    if (keyPos == std::string::npos) return {};
    const size_t colon = json.find(':', keyPos + needle.size());
    if (colon == std::string::npos) return {};
    const size_t quote = json.find('"', colon + 1);
    if (quote == std::string::npos) return {};
    std::string out;
    for (size_t i = quote + 1; i < json.size(); ++i) {
        const char c = json[i];
        if (c == '"') return out;
        if (c == '\\' && i + 1 < json.size()) {
            const char e = json[++i];
            if (e == '"' || e == '\\' || e == '/') out.push_back(e);
            else if (e == 'n') out.push_back('\n');
            else if (e == 'r') out.push_back('\r');
            else if (e == 't') out.push_back('\t');
            else return {};
        } else {
            out.push_back(c);
        }
    }
    return {};
}

static unsigned long long JsonUIntAfter(const std::string& json, const std::string& key, size_t start = 0) {
    const std::string needle = "\"" + key + "\"";
    const size_t keyPos = json.find(needle, start);
    if (keyPos == std::string::npos) return 0;
    const size_t colon = json.find(':', keyPos + needle.size());
    if (colon == std::string::npos) return 0;
    size_t p = colon + 1;
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t' || json[p] == '\r' || json[p] == '\n')) ++p;
    unsigned long long value = 0;
    bool any = false;
    while (p < json.size() && json[p] >= '0' && json[p] <= '9') {
        any = true;
        value = value * 10ULL + static_cast<unsigned long long>(json[p] - '0');
        ++p;
    }
    return any ? value : 0;
}

static std::vector<unsigned long long> ParseVersion(const std::wstring& text) {
    std::vector<unsigned long long> parts;
    unsigned long long value = 0;
    bool have = false;
    for (const wchar_t c : text) {
        if (c >= L'0' && c <= L'9') {
            have = true;
            value = value * 10ULL + static_cast<unsigned long long>(c - L'0');
        } else if (c == L'.') {
            parts.push_back(have ? value : 0);
            value = 0;
            have = false;
        } else {
            break;
        }
    }
    if (have || parts.empty()) parts.push_back(value);
    return parts;
}

static int CompareVersions(const std::wstring& left, const std::wstring& right) {
    const auto a = ParseVersion(left);
    const auto b = ParseVersion(right);
    const size_t count = std::max(a.size(), b.size());
    for (size_t i = 0; i < count; ++i) {
        const unsigned long long av = i < a.size() ? a[i] : 0;
        const unsigned long long bv = i < b.size() ? b[i] : 0;
        if (av < bv) return -1;
        if (av > bv) return 1;
    }
    return 0;
}

struct UpdateManifest {
    std::wstring version;
    std::wstring url;
    std::wstring sha256;
    unsigned long long size = 0;
};

static bool LoadUpdateManifest(UpdateManifest& manifest, std::wstring& error) {
    std::vector<unsigned char> bytes;
    if (!HttpGetBytes(kManifestUrl, bytes, error, 7000, 1024 * 1024)) return false;
    const std::string json(bytes.begin(), bytes.end());
    const size_t payloadPos = json.find("\"payload\"");
    if (payloadPos == std::string::npos) {
        error = L"Update-Manifest enthält keinen payload-Abschnitt.";
        return false;
    }
    manifest.version = Wide(JsonStringAfter(json, "version"));
    manifest.url = Wide(JsonStringAfter(json, "url", payloadPos));
    manifest.sha256 = Wide(JsonStringAfter(json, "sha256", payloadPos));
    manifest.size = JsonUIntAfter(json, "size", payloadPos);
    std::transform(manifest.sha256.begin(), manifest.sha256.end(), manifest.sha256.begin(), towlower);
    if (manifest.version.empty() || manifest.url.empty() || manifest.sha256.size() != 64 || manifest.size == 0) {
        error = L"Update-Manifest ist unvollständig oder ungültig.";
        return false;
    }
    URL_COMPONENTS parts{};
    std::vector<wchar_t> host, path;
    if (!CrackHttpsUrl(manifest.url, parts, host, path)) {
        error = L"Update-Paket verwendet keine sichere HTTPS-Adresse.";
        return false;
    }
    return true;
}

static bool RunHiddenAndWait(const std::wstring& command, const fs::path& cwd, DWORD& exitCode, std::wstring& error, DWORD timeoutMs) {
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
        cwd.empty() ? nullptr : cwd.c_str(), &si, &pi)) {
        error = L"Windows-Hilfsprozess konnte nicht gestartet werden. Fehler " + std::to_wstring(GetLastError()) + L".";
        return false;
    }
    CloseHandle(pi.hThread);
    const DWORD wait = WaitForSingleObject(pi.hProcess, timeoutMs);
    if (wait == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 124);
        CloseHandle(pi.hProcess);
        error = L"Ein Update-Schritt hat das Zeitlimit überschritten.";
        return false;
    }
    if (wait != WAIT_OBJECT_0) {
        CloseHandle(pi.hProcess);
        error = L"Ein Update-Schritt konnte nicht überwacht werden.";
        return false;
    }
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    return true;
}

static bool ExtractZip(const fs::path& zip, const fs::path& target, std::wstring& error) {
    wchar_t systemDir[MAX_PATH]{};
    if (!GetSystemDirectoryW(systemDir, MAX_PATH)) {
        error = L"Windows-Systemordner konnte nicht ermittelt werden.";
        return false;
    }
    const fs::path tar = fs::path(systemDir) / L"tar.exe";
    if (!fs::exists(tar)) {
        error = L"Windows-Systemwerkzeug tar.exe ist nicht verfügbar.";
        return false;
    }
    std::error_code ec;
    fs::remove_all(target, ec);
    ec.clear();
    fs::create_directories(target, ec);
    if (ec) {
        error = L"Update-Staging-Verzeichnis konnte nicht erstellt werden.";
        return false;
    }
    DWORD code = 0;
    if (!RunHiddenAndWait(Quote(tar.wstring()) + L" -xf " + Quote(zip.wstring()) + L" -C " + Quote(target.wstring()),
        target, code, error, 120000)) return false;
    if (code != 0) {
        error = L"Update-Paket konnte nicht entpackt werden (tar Exitcode " + std::to_wstring(code) + L").";
        return false;
    }
    return true;
}

static bool ValidateStage(const fs::path& stage, std::wstring& error) {
    const fs::path required[] = {
        stage / L"SyntexLauncher.exe",
        stage / L"app" / L"SyntexLauncher.exe",
        stage / L"app" / L"SyntexUninstall.exe",
        stage / L"app" / L"SyntexUpdateAgent.exe"
    };
    for (const auto& path : required) {
        if (!fs::exists(path) || !fs::is_regular_file(path)) {
            error = L"Update-Paket ist unvollständig: " + path.filename().wstring();
            return false;
        }
    }
    return true;
}

static bool StartUpdateAgent(const fs::path& root, const fs::path& stage, const std::wstring& targetVersion, std::wstring& error) {
    const fs::path stagedAgent = stage / L"app" / L"SyntexUpdateAgent.exe";
    wchar_t tempPath[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, tempPath)) {
        error = L"Windows-Temp-Verzeichnis konnte nicht ermittelt werden.";
        return false;
    }
    const fs::path tempAgent = fs::path(tempPath) /
        (L"SyntexUpdateAgent-" + std::to_wstring(GetCurrentProcessId()) + L"-" + Timestamp() + L".exe");
    std::error_code ec;
    fs::copy_file(stagedAgent, tempAgent, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        error = L"Update-Agent konnte nicht vorbereitet werden.";
        return false;
    }

    std::wstring command = Quote(tempAgent.wstring()) +
        L" --root " + Quote(root.wstring()) +
        L" --stage " + Quote(stage.wstring()) +
        L" --parent-pid " + std::to_wstring(GetCurrentProcessId()) +
        L" --from-version " + Quote(kCurrentVersion) +
        L" --to-version " + Quote(targetVersion);
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(tempAgent.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS,
        nullptr, root.c_str(), &si, &pi)) {
        error = L"Update-Agent konnte nicht gestartet werden. Windows-Fehler: " + std::to_wstring(GetLastError());
        fs::remove(tempAgent, ec);
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

static bool CheckAndStageUpdate(const fs::path& root, std::wstring& error) {
    UpdateManifest manifest;
    if (!LoadUpdateManifest(manifest, error)) return false;
    if (CompareVersions(kCurrentVersion, manifest.version) >= 0) {
        AppendUpdateLog(root, L"Version " + std::wstring(kCurrentVersion) + L" ist aktuell. Server: " + manifest.version + L".");
        return false;
    }

    AppendUpdateLog(root, L"Neue Version gefunden: " + manifest.version + L". Download wird vorbereitet.");
    const fs::path updateRoot = root / L"storage" / L"cache" / L"launcher-update";
    const fs::path versionRoot = updateRoot / manifest.version;
    const fs::path zip = versionRoot / L"payload.zip";
    const fs::path stage = versionRoot / L"stage";
    std::error_code ec;
    fs::create_directories(versionRoot, ec);
    if (ec) {
        error = L"Update-Cache konnte nicht erstellt werden.";
        return false;
    }

    bool needDownload = true;
    if (fs::exists(zip) && fs::is_regular_file(zip)) {
        const auto existingSize = fs::file_size(zip, ec);
        if (!ec && existingSize == manifest.size) {
            std::wstring existingHash;
            std::wstring hashError;
            if (Sha256File(zip, existingHash, hashError) && _wcsicmp(existingHash.c_str(), manifest.sha256.c_str()) == 0)
                needDownload = false;
        }
    }

    if (needDownload) {
        fs::remove(zip, ec);
        if (!HttpDownloadFile(manifest.url, zip, manifest.size, error)) return false;
    }

    std::wstring hash;
    if (!Sha256File(zip, hash, error)) return false;
    if (_wcsicmp(hash.c_str(), manifest.sha256.c_str()) != 0) {
        fs::remove(zip, ec);
        error = L"Update wurde wegen einer ungültigen SHA-256-Prüfsumme verworfen.";
        return false;
    }

    if (!ExtractZip(zip, stage, error)) return false;
    if (!ValidateStage(stage, error)) return false;
    if (!StartUpdateAgent(root, stage, manifest.version, error)) return false;
    AppendUpdateLog(root, L"Update " + manifest.version + L" wurde geprüft und an den Update-Agent übergeben.");
    return true;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    const fs::path exe = ExePath();
    if (exe.empty()) return 90;
    const fs::path root = exe.parent_path();
    const fs::path child = root / L"app" / L"SyntexLauncher.exe";
    auto args = Arguments();

    if (args.size() == 1 && _wcsicmp(args[0].c_str(), L"--host-log-self-test") == 0) {
        const auto log = WriteLog(root, L"Syntex Launcher 0.16.7 native startup logging self-test\r\n");
        return log.empty() ? 91 : 0;
    }

    const bool ciGui = args.size() == 1 && _wcsicmp(args[0].c_str(), L"--ci-gui-self-test") == 0;
    const bool healthCheck = HasArg(args, L"--post-update-health-check");
    const bool rollbackNotice = HasArg(args, L"--update-rollback-notice");

    if (!ciGui && !healthCheck && !rollbackNotice) {
        std::wstring updateError;
        if (CheckAndStageUpdate(root, updateError)) return 0;
        if (!updateError.empty()) {
            AppendUpdateLog(root, L"Updateprüfung ohne Blockierung beendet: " + updateError);
        }
    }

    if (rollbackNotice) {
        MessageBoxW(nullptr,
            L"Eine neue Syntex-Launcher-Version konnte nicht zuverlässig gestartet werden.\n\n"
            L"Die vorherige funktionierende Version wurde automatisch wiederhergestellt.",
            L"Syntex Launcher – Update zurückgesetzt", MB_OK | MB_ICONWARNING);
    }

    if (!fs::exists(child)) {
        const auto log = WriteLog(root, L"Startfehler: app\\SyntexLauncher.exe fehlt.\r\nRoot: " + root.wstring() + L"\r\n");
        MessageBoxW(nullptr, (L"Syntex Launcher ist unvollständig.\n\nDiagnose: " + log.wstring()).c_str(), L"Syntex Launcher – Startfehler", MB_OK | MB_ICONERROR);
        return 92;
    }

    std::error_code ec;
    fs::create_directories(root / L"storage", ec);
    SetEnvironmentVariableW(L"SYNTEX_ROOT", (root / L"storage").c_str());

    std::wstring command = Quote(child.wstring());
    if (!ciGui && !healthCheck && !rollbackNotice) {
        for (const auto& arg : args) command += L" " + Quote(arg);
    }
    std::vector<wchar_t> mutableCmd(command.begin(), command.end());
    mutableCmd.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(child.c_str(), mutableCmd.data(), nullptr, nullptr, FALSE, 0, nullptr, root.c_str(), &si, &pi)) {
        const DWORD e = GetLastError();
        const auto log = WriteLog(root, L"CreateProcessW fehlgeschlagen.\r\nWindows-Fehler: " + std::to_wstring(e) + L"\r\n" + WinError(e) + L"\r\nChild: " + child.wstring() + L"\r\n");
        MessageBoxW(nullptr, (L"Syntex Launcher konnte nicht gestartet werden.\n\nDiagnose: " + log.wstring()).c_str(), L"Syntex Launcher – Startfehler", MB_OK | MB_ICONERROR);
        return 93;
    }
    CloseHandle(pi.hThread);

    const auto timeout = ciGui || healthCheck ? std::chrono::seconds(120) : std::chrono::seconds(30);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const DWORD wait = WaitForSingleObject(pi.hProcess, 100);
        if (wait == WAIT_OBJECT_0) {
            DWORD exitCode = 0;
            GetExitCodeProcess(pi.hProcess, &exitCode);
            CloseHandle(pi.hProcess);
            const auto log = WriteLog(root, L"Launcher-Prozess wurde während der Startprüfung beendet.\r\nExitcode: " + std::to_wstring(exitCode) + L"\r\nChild: " + child.wstring() + L"\r\n");
            if (!ciGui && !healthCheck)
                MessageBoxW(nullptr, (L"Syntex Launcher wurde beim Start unerwartet beendet.\n\nExitcode: " + std::to_wstring(exitCode) + L"\nDiagnose: " + log.wstring()).c_str(), L"Syntex Launcher – Startfehler", MB_OK | MB_ICONERROR);
            return exitCode == 0 ? 94 : static_cast<int>(exitCode & 0xFFu);
        }
        if (HasVisibleWindow(pi.dwProcessId)) {
            const auto log = WriteLog(root, L"Startprüfung erfolgreich: sichtbares Launcher-Fenster erkannt.\r\nPID: " + std::to_wstring(pi.dwProcessId) + L"\r\nVersion: " + std::wstring(kCurrentVersion) + L"\r\n");
            if (ciGui || healthCheck) {
                if (ciGui) {
                    TerminateProcess(pi.hProcess, 0);
                    WaitForSingleObject(pi.hProcess, 5000);
                }
            }
            CloseHandle(pi.hProcess);
            return log.empty() ? 95 : 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (ciGui || healthCheck) {
        TerminateProcess(pi.hProcess, 96);
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hProcess);
        WriteLog(root, L"Startprüfung fehlgeschlagen: innerhalb von 120 Sekunden kein sichtbares Fenster.\r\n");
        return 96;
    }

    WriteLog(root, L"Startprüfung fehlgeschlagen: innerhalb von 30 Sekunden kein sichtbares Fenster. Prozess wird beendet.\r\nPID: " + std::to_wstring(pi.dwProcessId) + L"\r\n");
    TerminateProcess(pi.hProcess, 97);
    WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hProcess);
    MessageBoxW(nullptr,
        L"Syntex Launcher konnte innerhalb von 30 Sekunden kein sichtbares Fenster öffnen.\n\n"
        L"Der Start wurde beendet. Details stehen unter storage\\logs\\launcher-startup.",
        L"Syntex Launcher – Startfehler", MB_OK | MB_ICONERROR);
    return 97;
}

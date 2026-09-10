#pragma once

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace SyntexSetupRepair {
namespace fs = std::filesystem;

struct Transaction {
    bool repair = false;
    bool prepared = false;
    fs::path backupPath;
    std::vector<fs::path> oldEntries;
    std::vector<fs::path> newEntryNames;
};

inline bool EqualName(const fs::path& path, const wchar_t* expected) {
    std::wstring left = path.filename().wstring();
    std::wstring right(expected ? expected : L"");
    if (left.size() != right.size()) return false;
    return std::equal(left.begin(), left.end(), right.begin(), [](wchar_t a, wchar_t b) {
        return std::towlower(a) == std::towlower(b);
    });
}

inline bool IsPreservedUserEntry(const fs::path& path) {
    static constexpr const wchar_t* preserved[] = {
        // Current Syntex-owned persistent root. It may contain settings, accounts,
        // logs and other player/creator state and must never be treated as program payload.
        L"storage",
        L"data", L"instances", L"runtimes",
        L"cache", L"downloads", L"logs", L"creator", L"diagnostics", L"temp", L"updates", L"fehleranalysen",
        // Legacy/user-owned layouts that must never be discarded by an in-place setup repair.
        L"saves", L"screenshots", L"resourcepacks", L"shaderpacks", L"config", L"profiles", L"accounts", L"backups"
    };
    for (const auto* value : preserved) {
        if (EqualName(path, value)) return true;
    }
    const auto name = path.filename().wstring();
    return name.rfind(L".syntex-repair-backup", 0) == 0;
}

inline void CollectStageEntryNames(const fs::path& stage, Transaction& transaction, std::error_code& ec) {
    transaction.newEntryNames.clear();
    for (const auto& entry : fs::directory_iterator(stage, ec)) {
        if (ec) return;
        transaction.newEntryNames.push_back(entry.path().filename());
    }
}

inline bool RestoreMovedEntries(const fs::path& root, Transaction& transaction, std::wstring& error) {
    std::error_code ec;
    bool ok = true;
    std::wstring details;

    for (auto it = transaction.oldEntries.rbegin(); it != transaction.oldEntries.rend(); ++it) {
        const fs::path source = transaction.backupPath / *it;
        const fs::path destination = root / *it;
        if (!fs::exists(source, ec)) {
            ec.clear();
            continue;
        }
        fs::rename(source, destination, ec);
        if (ec) {
            ok = false;
            if (!details.empty()) details += L", ";
            details += it->wstring();
            ec.clear();
        }
    }

    if (ok) {
        fs::remove_all(transaction.backupPath, ec);
        transaction.prepared = false;
        transaction.oldEntries.clear();
    } else {
        error = L"Rollback konnte folgende vorherige Programmdateien nicht wiederherstellen: " + details + L". Backup: " + transaction.backupPath.wstring();
    }
    return ok;
}

inline bool Begin(const fs::path& root, const fs::path& stage, Transaction& transaction, std::wstring& error) {
    std::error_code ec;
    transaction = {};
    CollectStageEntryNames(stage, transaction, ec);
    if (ec || transaction.newEntryNames.empty()) {
        error = L"Temporäre Installation konnte für die Reparaturtransaktion nicht gelesen werden.";
        return false;
    }

    transaction.repair = fs::exists(root / L"SyntexLauncher.exe", ec) || fs::exists(root / L"app", ec);
    ec.clear();
    if (!transaction.repair) {
        transaction.prepared = true;
        return true;
    }

    std::vector<fs::path> candidates;
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (ec) {
            error = L"Vorhandene Installation konnte nicht gelesen werden.";
            return false;
        }
        if (IsPreservedUserEntry(entry.path())) continue;
        candidates.push_back(entry.path().filename());
    }

    transaction.backupPath = root / L".syntex-repair-backup";
    if (fs::exists(transaction.backupPath, ec)) {
        error = L"Eine vorherige Reparatursicherung ist noch vorhanden und wird aus Sicherheitsgründen nicht überschrieben: " + transaction.backupPath.wstring();
        return false;
    }
    fs::create_directory(transaction.backupPath, ec);
    if (ec) {
        error = L"Reparatursicherung konnte nicht angelegt werden.";
        return false;
    }

    transaction.prepared = true;
    for (const auto& name : candidates) {
        const fs::path source = root / name;
        const fs::path destination = transaction.backupPath / name;
        fs::rename(source, destination, ec);
        if (ec) {
            std::wstring restoreError;
            RestoreMovedEntries(root, transaction, restoreError);
            error = L"Vorhandene Programmdateien konnten nicht in die Reparatursicherung verschoben werden. Bitte Syntex Launcher vollständig schließen und Setup erneut starten. Betroffen: " + name.wstring();
            if (!restoreError.empty()) error += L" " + restoreError;
            return false;
        }
        transaction.oldEntries.push_back(name);
    }
    return true;
}

inline bool Rollback(const fs::path& root, Transaction& transaction, std::wstring& error) {
    if (!transaction.prepared) return true;

    std::error_code ec;
    for (const auto& name : transaction.newEntryNames) {
        fs::remove_all(root / name, ec);
        if (ec) {
            error = L"Rollback konnte neue Programmdateien nicht entfernen: " + name.wstring();
            return false;
        }
    }

    if (!transaction.repair) {
        transaction.prepared = false;
        return true;
    }
    return RestoreMovedEntries(root, transaction, error);
}

inline bool Commit(Transaction& transaction, std::wstring& error) {
    if (!transaction.prepared) return true;
    if (!transaction.repair) {
        transaction.prepared = false;
        return true;
    }

    std::error_code ec;
    fs::remove_all(transaction.backupPath, ec);
    if (ec) {
        error = L"Die alte Programmsicherung konnte nach erfolgreicher Startprüfung nicht entfernt werden: " + transaction.backupPath.wstring();
        return false;
    }
    transaction.prepared = false;
    transaction.oldEntries.clear();
    return true;
}

inline std::string ReadSmallTextFile(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

inline bool RunFilesystemSelfTest() {
    std::error_code ec;
    const auto unique = std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count());
    const fs::path base = fs::temp_directory_path() / (L"SyntexRepairTransactionTest-" + unique);
    const fs::path stage = base / L"stage";
    const fs::path successRoot = base / L"success";
    const fs::path rollbackRoot = base / L"rollback";
    fs::remove_all(base, ec);

    auto seedRoot = [&](const fs::path& root) {
        fs::create_directories(root / L"storage" / L"data" / L"settings", ec);
        fs::create_directories(root / L"data" / L"settings", ec);
        fs::create_directories(root / L"instances" / L"world-a", ec);
        fs::create_directories(root / L"runtimes" / L"java-21", ec);
        fs::create_directories(root / L"screenshots", ec);
        fs::create_directories(root / L"tools", ec);
        std::ofstream(root / L"SyntexLauncher.exe") << "old-launcher";
        std::ofstream(root / L"Old.Managed.dll") << "old-dll";
        std::ofstream(root / L"tools" / L"old-tool.bin") << "old-tool";
        std::ofstream(root / L"storage" / L"data" / L"settings" / L"settings.json") << "keep-storage";
        std::ofstream(root / L"data" / L"settings" / L"settings.json") << "keep-data";
        std::ofstream(root / L"instances" / L"world-a" / L"level.dat") << "keep-instance";
        std::ofstream(root / L"runtimes" / L"java-21" / L"release") << "keep-runtime";
        std::ofstream(root / L"screenshots" / L"legacy.png") << "keep-legacy";
    };

    fs::create_directories(stage / L"app", ec);
    if (ec) return false;
    std::ofstream(stage / L"SyntexLauncher.exe") << "new-launcher";
    std::ofstream(stage / L"app" / L"SyntexLauncher.App.exe") << "new-app";
    std::ofstream(stage / L"app" / L"SyntexUninstall.exe") << "new-uninstaller";

    seedRoot(successRoot);
    Transaction success;
    std::wstring error;
    if (!Begin(successRoot, stage, success, error) || !success.repair) { fs::remove_all(base, ec); return false; }
    for (const auto& entry : fs::directory_iterator(stage, ec)) {
        if (ec) { fs::remove_all(base, ec); return false; }
        fs::copy(entry.path(), successRoot / entry.path().filename(), fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        if (ec) { fs::remove_all(base, ec); return false; }
    }
    if (!Commit(success, error)) { fs::remove_all(base, ec); return false; }
    const bool successOk =
        ReadSmallTextFile(successRoot / L"SyntexLauncher.exe") == "new-launcher" &&
        fs::exists(successRoot / L"app" / L"SyntexLauncher.App.exe") &&
        !fs::exists(successRoot / L"Old.Managed.dll") &&
        !fs::exists(successRoot / L"tools") &&
        ReadSmallTextFile(successRoot / L"storage" / L"data" / L"settings" / L"settings.json") == "keep-storage" &&
        ReadSmallTextFile(successRoot / L"data" / L"settings" / L"settings.json") == "keep-data" &&
        ReadSmallTextFile(successRoot / L"instances" / L"world-a" / L"level.dat") == "keep-instance" &&
        ReadSmallTextFile(successRoot / L"runtimes" / L"java-21" / L"release") == "keep-runtime" &&
        ReadSmallTextFile(successRoot / L"screenshots" / L"legacy.png") == "keep-legacy";

    seedRoot(rollbackRoot);
    Transaction rollback;
    error.clear();
    if (!Begin(rollbackRoot, stage, rollback, error) || !rollback.repair) { fs::remove_all(base, ec); return false; }
    for (const auto& entry : fs::directory_iterator(stage, ec)) {
        if (ec) { fs::remove_all(base, ec); return false; }
        fs::copy(entry.path(), rollbackRoot / entry.path().filename(), fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        if (ec) { fs::remove_all(base, ec); return false; }
    }

    // Simulate a failed post-copy launcher health check and execute the real rollback path.
    error.clear();
    const bool rollbackCallOk = Rollback(rollbackRoot, rollback, error);
    const bool rollbackOk = rollbackCallOk &&
        ReadSmallTextFile(rollbackRoot / L"SyntexLauncher.exe") == "old-launcher" &&
        ReadSmallTextFile(rollbackRoot / L"Old.Managed.dll") == "old-dll" &&
        ReadSmallTextFile(rollbackRoot / L"tools" / L"old-tool.bin") == "old-tool" &&
        !fs::exists(rollbackRoot / L"app") &&
        ReadSmallTextFile(rollbackRoot / L"storage" / L"data" / L"settings" / L"settings.json") == "keep-storage" &&
        ReadSmallTextFile(rollbackRoot / L"data" / L"settings" / L"settings.json") == "keep-data" &&
        ReadSmallTextFile(rollbackRoot / L"instances" / L"world-a" / L"level.dat") == "keep-instance" &&
        ReadSmallTextFile(rollbackRoot / L"runtimes" / L"java-21" / L"release") == "keep-runtime" &&
        ReadSmallTextFile(rollbackRoot / L"screenshots" / L"legacy.png") == "keep-legacy" &&
        !fs::exists(rollback.backupPath);

    fs::remove_all(base, ec);
    return successOk && rollbackOk;
}

} // namespace SyntexSetupRepair

#include "SteamDiscovery.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <regex>
#include <system_error>
#include <unordered_set>

namespace {
std::optional<std::filesystem::path> steamInstallDirectory() {
    struct RegistryLocation {
        HKEY root;
        const wchar_t* value;
        DWORD flags;
    };
    const RegistryLocation locations[] = {
        {HKEY_CURRENT_USER, L"SteamPath", 0},
        {HKEY_CURRENT_USER, L"InstallPath", 0},
        {HKEY_LOCAL_MACHINE, L"InstallPath", RRF_SUBKEY_WOW6432KEY},
        {HKEY_LOCAL_MACHINE, L"InstallPath", RRF_SUBKEY_WOW6464KEY},
    };
    for (const auto& location : locations) {
        wchar_t buffer[32768]{};
        DWORD size = sizeof(buffer);
        const LSTATUS status = RegGetValueW(location.root, L"Software\\Valve\\Steam", location.value,
                                            RRF_RT_REG_SZ | location.flags, nullptr, buffer, &size);
        if (status == ERROR_SUCCESS && std::filesystem::exists(buffer)) {
            return std::filesystem::path(buffer);
        }
    }
    return std::nullopt;
}

std::string readBytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::wstring decodeText(const std::string& bytes) {
    if (bytes.empty()) return {};
    UINT codePage = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    int length = MultiByteToWideChar(codePage, flags, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    if (length == 0) {
        codePage = CP_ACP;
        flags = 0;
        length = MultiByteToWideChar(codePage, flags, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    }
    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(codePage, flags, bytes.data(), static_cast<int>(bytes.size()), result.data(), length);
    return result;
}

std::wstring unescapeVdf(std::wstring value) {
    std::wstring result;
    result.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == L'\\' && i + 1 < value.size() && value[i + 1] == L'\\') ++i;
        result.push_back(value[i]);
    }
    return result;
}

std::optional<std::wstring> vdfValue(const std::wstring& text, const std::wstring& wantedKey) {
    static const std::wregex pairPattern(LR"vdf("([^"]+)"\s*"([^"]*)")vdf");
    for (std::wsregex_iterator it(text.begin(), text.end(), pairPattern), end; it != end; ++it) {
        if (_wcsicmp((*it)[1].str().c_str(), wantedKey.c_str()) == 0) {
            return unescapeVdf((*it)[2].str());
        }
    }
    return std::nullopt;
}

std::wstring normalizedPath(const std::filesystem::path& path, bool directory) {
    std::error_code error;
    auto normalized = std::filesystem::weakly_canonical(path, error);
    if (error) normalized = path.lexically_normal();
    std::wstring value = normalized.wstring();
    std::replace(value.begin(), value.end(), L'/', L'\\');
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); });
    if (directory && !value.empty() && value.back() != L'\\') value.push_back(L'\\');
    return value;
}

std::vector<std::filesystem::path> libraryDirectories(const std::filesystem::path& steamDirectory) {
    std::vector<std::filesystem::path> libraries{steamDirectory};
    const auto text = decodeText(readBytes(steamDirectory / L"steamapps" / L"libraryfolders.vdf"));
    static const std::wregex pairPattern(LR"vdf("([^"]+)"\s*"([^"]*)")vdf");
    for (std::wsregex_iterator it(text.begin(), text.end(), pairPattern), end; it != end; ++it) {
        if (_wcsicmp((*it)[1].str().c_str(), L"path") == 0) {
            libraries.emplace_back(unescapeVdf((*it)[2].str()));
        }
    }

    std::unordered_set<std::wstring> seen;
    std::vector<std::filesystem::path> unique;
    for (const auto& library : libraries) {
        if (seen.insert(normalizedPath(library, false)).second) unique.push_back(library);
    }
    return unique;
}
} // namespace

bool SteamDiscovery::refresh() {
    games_.clear();
    lastError_.clear();
    const auto steamDirectory = steamInstallDirectory();
    if (!steamDirectory) {
        lastError_ = L"Steam nao encontrada no Registro do usuario.";
        return false;
    }

    std::unordered_set<std::wstring> seenDirectories;
    for (const auto& library : libraryDirectories(*steamDirectory)) {
        const auto steamApps = library / L"steamapps";
        std::error_code error;
        std::filesystem::directory_iterator entries(steamApps, error);
        if (error) continue;

        for (const auto& entry : entries) {
            if (!entry.is_regular_file(error)) continue;
            const std::wstring filename = entry.path().filename().wstring();
            if (!filename.starts_with(L"appmanifest_") || entry.path().extension() != L".acf") continue;

            const std::wstring manifest = decodeText(readBytes(entry.path()));
            const auto installDir = vdfValue(manifest, L"installdir");
            if (!installDir || installDir->empty()) continue;

            const auto directory = steamApps / L"common" / *installDir;
            const std::wstring normalized = normalizedPath(directory, true);
            if (!seenDirectories.insert(normalized).second) continue;

            std::wstring appId = filename.substr(12, filename.size() - 12 - 4);
            std::wstring name = vdfValue(manifest, L"name").value_or(*installDir);
            games_.push_back({std::move(appId), std::move(name), directory});
        }
    }

    if (games_.empty()) {
        lastError_ = L"Nenhum appmanifest da Steam foi encontrado.";
        return false;
    }
    return true;
}

std::optional<SteamGame> SteamDiscovery::gameForExecutable(const std::filesystem::path& executable) const {
    const std::wstring candidate = normalizedPath(executable, false);
    for (const auto& game : games_) {
        const std::wstring root = normalizedPath(game.installDirectory, true);
        if (candidate.starts_with(root)) return game;
    }
    return std::nullopt;
}

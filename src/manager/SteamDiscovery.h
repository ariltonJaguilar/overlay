#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

struct SteamGame {
    std::wstring appId;
    std::wstring name;
    std::filesystem::path installDirectory;
};

class SteamDiscovery {
public:
    bool refresh();
    std::optional<SteamGame> gameForExecutable(const std::filesystem::path& executable) const;
    const std::vector<SteamGame>& games() const { return games_; }
    const std::wstring& lastError() const { return lastError_; }

private:
    std::vector<SteamGame> games_;
    std::wstring lastError_;
};


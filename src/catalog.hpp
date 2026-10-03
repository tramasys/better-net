#pragma once
#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace bnet
{
struct Game
{
    std::wstring product_id;
    std::wstring title;
    std::wstring version;
    std::filesystem::path install_path;
    std::filesystem::path executable;
    std::filesystem::path artwork_path;
    std::wstring launch_code;
};

struct Catalog
{
    std::vector<Game> games;
    std::filesystem::path client_path;
    std::vector<std::wstring> warnings;
    bool scan_failed = false;
};

struct InstallationRecord
{
    std::wstring product_id;
    std::filesystem::path install_path;
    std::wstring display_name;
};

struct AgentRecords
{
    std::vector<InstallationRecord> records;
    bool malformed = false;
};

Catalog discover(std::stop_token cancellation = {});
bool validate_game(const Game& game);
std::optional<std::wstring> launch_code_for(std::wstring_view product_id);
std::optional<std::wstring_view> direct_launch_arguments_for(std::wstring_view product_id);
AgentRecords parse_agent_records(std::span<const std::byte> data);
std::wstring read_build_version(const std::filesystem::path& manifest, std::wstring_view product_id);

Catalog catalog_from_records(std::span<const InstallationRecord> records,
                             const std::filesystem::path& client_path = {},
                             std::stop_token cancellation = {});
}

#include "catalog.hpp"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <fstream>
#include <limits>
#include <memory>
#include <system_error>
#include <type_traits>
#include <utility>

namespace bnet {
namespace {
constexpr std::size_t max_database_bytes = 4 * 1024 * 1024;
constexpr std::size_t max_manifest_bytes = 1024 * 1024;
constexpr std::size_t max_records = 4096;

struct Product {
    std::wstring_view id;
    std::wstring_view title;
    std::wstring_view launch_code;
    std::array<std::wstring_view, 3> executables;
    std::wstring_view versioned_executable{};
    std::optional<std::wstring_view> direct_arguments{};
};

constexpr std::array products = {
    Product{L"auks", L"Call of Duty", L"AUKS", {L"_retail_/cod.exe", L"cod.exe", L""}},
    Product{L"fen", L"Diablo IV", L"Fen", {L"Diablo IV.exe", L"", L""}},
    Product{L"prometheus", L"Overwatch 2", L"Pro", {L"_retail_/Overwatch.exe", L"Overwatch.exe", L""}},
    Product{L"wow", L"World of Warcraft", L"WoW", {L"_retail_/Wow.exe", L"Wow.exe", L""}, {}, L""},
    Product{L"diablo3", L"Diablo III", L"D3", {L"x64/Diablo III64.exe", L"Diablo III.exe", L""}, {}, L"-launch"},
    Product{L"osi", L"Diablo II: Resurrected", L"OSI", {L"D2R.exe", L"", L""}},
    Product{L"hs_beta", L"Hearthstone", L"WTCG", {L"Hearthstone.exe", L"", L""}},
    Product{L"heroes", L"Heroes of the Storm", L"Hero", {}, L"HeroesOfTheStorm_x64.exe"},
    Product{L"s1", L"StarCraft", L"S1", {L"x86_64/StarCraft.exe", L"x86/StarCraft.exe", L"StarCraft.exe"}},
    Product{L"s2", L"StarCraft II", L"S2", {}, L"SC2_x64.exe"},
    Product{L"w3", L"Warcraft III: Reforged", L"W3", {L"_retail_/x86_64/Warcraft III.exe", L"", L""}},
    Product{L"odin", L"Call of Duty: Modern Warfare", L"ODIN", {L"ModernWarfare.exe", L"", L""}},
    Product{L"viper", L"Call of Duty: Black Ops 4", L"VIPR", {L"BlackOps4.exe", L"", L""}},
    Product{L"zeus", L"Call of Duty: Black Ops Cold War", L"ZEUS", {L"BlackOpsColdWar.exe", L"", L""}},
    Product{L"fore", L"Call of Duty: Vanguard", L"FORE", {L"Vanguard.exe", L"", L""}}};

std::wstring lower(std::wstring_view value) {
    std::wstring result(value);

    for (auto& character : result) {
        character = static_cast<wchar_t>(std::towlower(character));
    }

    return result;
}

const Product* product_for(std::wstring_view id) {
    const auto normalized = lower(id);
    const auto found = std::ranges::find(products, normalized, &Product::id);
    return found == products.end() ? nullptr : &*found;
}

std::wstring utf8(std::string_view value) {
    if (value.empty() || value.size() > 32768 || value.find('\0') != std::string_view::npos) {
        return {};
    }

    const auto size = static_cast<int>(value.size());
    const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), size, nullptr, 0);

    if (required <= 0) {
        return {};
    }

    std::wstring result(static_cast<std::size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), size, result.data(), required);
    return result;
}

std::optional<std::string> read_bounded(const std::filesystem::path& path, std::size_t maximum) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);

    if (error || size > maximum) {
        return std::nullopt;
    }

    std::ifstream stream(path, std::ios::binary);

    if (!stream) {
        return std::nullopt;
    }

    std::string contents(static_cast<std::size_t>(size), '\0');
    stream.read(contents.data(), static_cast<std::streamsize>(contents.size()));

    if (stream.gcount() != static_cast<std::streamsize>(contents.size()) || stream.peek() != EOF) {
        return std::nullopt;
    }

    return contents;
}

bool regular_file(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error);
}

bool executable_file(const std::filesystem::path& path) {
    if (!regular_file(path)) {
        return false;
    }

    std::ifstream stream(path, std::ios::binary);
    std::array<unsigned char, 64> header{};
    stream.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));

    if (!stream || header[0] != 'M' || header[1] != 'Z') {
        return false;
    }

    const std::uint32_t offset = static_cast<std::uint32_t>(header[60]) | (static_cast<std::uint32_t>(header[61]) << 8) |
                                 (static_cast<std::uint32_t>(header[62]) << 16) | (static_cast<std::uint32_t>(header[63]) << 24);

    if (offset < 64 || offset > max_manifest_bytes) {
        return false;
    }

    stream.seekg(static_cast<std::streamoff>(offset));
    std::array<unsigned char, 24> pe{};
    stream.read(reinterpret_cast<char*>(pe.data()), static_cast<std::streamsize>(pe.size()));

    return stream && pe[0] == 'P' && pe[1] == 'E' && pe[2] == 0 && pe[3] == 0 && (pe[22] & IMAGE_FILE_EXECUTABLE_IMAGE) != 0 &&
           (pe[23] & 0x20) == 0;
}

std::filesystem::path normalized_path(const std::filesystem::path& value) {
    std::error_code error;
    auto normalized = std::filesystem::weakly_canonical(value, error);
    return error ? value.lexically_normal() : normalized;
}

bool same_path(const std::filesystem::path& first, const std::filesystem::path& second) {
    const auto left = normalized_path(first).wstring();
    const auto right = normalized_path(second).wstring();

    return CompareStringOrdinal(left.c_str(), static_cast<int>(left.size()), right.c_str(), static_cast<int>(right.size()), TRUE) ==
           CSTR_EQUAL;
}

std::filesystem::path game_executable(const Product& product, const std::filesystem::path& directory) {
    for (const auto relative : product.executables) {
        if (!relative.empty() && executable_file(directory / relative)) {
            return directory / relative;
        }
    }

    if (product.versioned_executable.empty()) {
        return {};
    }

    std::error_code error;
    const auto versions = directory / L"Versions";
    const auto entries = std::filesystem::directory_iterator(versions, error);

    if (error) {
        return {};
    }

    std::filesystem::path selected;
    std::uint64_t selected_build = 0;
    std::size_t examined = 0;

    for (auto entry = entries; entry != std::filesystem::directory_iterator{} && examined < 256; entry.increment(error), ++examined) {
        if (error) {
            break;
        }

        const auto name = entry->path().filename().wstring();

        if (!name.starts_with(L"Base") || name.size() <= 4 || name.size() > 16) {
            continue;
        }

        std::uint64_t build = 0;
        bool numeric = true;

        for (const auto character : std::wstring_view(name).substr(4)) {
            if (character < L'0' || character > L'9') {
                numeric = false;
                break;
            }

            build = build * 10 + static_cast<std::uint64_t>(character - L'0');
        }

        const auto candidate = entry->path() / product.versioned_executable;

        if (numeric && build > selected_build && executable_file(candidate)) {
            selected_build = build;
            selected = candidate;
        }
    }

    return selected;
}

std::wstring executable_version(const std::filesystem::path& path) {
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);

    if (size == 0 || size > max_manifest_bytes) {
        return {};
    }

    std::vector<std::byte> data(size);

    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) {
        return {};
    }

    VS_FIXEDFILEINFO* info = nullptr;
    UINT length = 0;

    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &length) || length < sizeof(VS_FIXEDFILEINFO) ||
        info->dwSignature != 0xfeef04bd) {
        return {};
    }

    const auto high = info->dwFileVersionMS;
    const auto low = info->dwFileVersionLS;

    if (high == 0 && low == 0) {
        return {};
    }

    return std::to_wstring(HIWORD(high)) + L"." + std::to_wstring(LOWORD(high)) + L"." + std::to_wstring(HIWORD(low)) + L"." +
           std::to_wstring(LOWORD(low));
}

std::vector<std::string_view> split(std::string_view line, char separator, std::size_t maximum = 64) {
    std::vector<std::string_view> fields;
    std::size_t start = 0;

    while (start <= line.size() && fields.size() < maximum) {
        const auto end = line.find(separator, start);
        fields.push_back(line.substr(start, end == std::string_view::npos ? end : end - start));

        if (end == std::string_view::npos) {
            return fields;
        }

        start = end + 1;
    }

    return {};
}

bool valid_version(std::string_view version) {
    return !version.empty() && version.size() <= 80 && std::ranges::all_of(version, [](unsigned char character) {
        return (character >= '0' && character <= '9') || character == '.' || character == '-' || character == '_' ||
               (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z');
    }) && std::ranges::any_of(version, [](unsigned char character) {
        return character >= '0' && character <= '9';
    });
}

struct WireField {
    std::uint32_t number = 0;
    unsigned type = 0;
    std::span<const std::byte> bytes;
};

class WireReader {
public:
    explicit WireReader(std::span<const std::byte> bytes) : bytes_(bytes) {
    }

    bool next(WireField& field) {
        std::uint64_t tag = 0;

        if (!varint(tag) || (tag >> 3) == 0 || (tag >> 3) > 536870911) {
            failed_ = true;
            return false;
        }

        field.number = static_cast<std::uint32_t>(tag >> 3);
        field.type = static_cast<unsigned>(tag & 7);
        field.bytes = {};
        std::uint64_t length = 0;

        if (field.type == 0) {
            if (!varint(length)) {
                failed_ = true;
                return false;
            }

            return true;
        }

        if (field.type == 1) {
            length = 8;
        } else if (field.type == 5) {
            length = 4;
        } else if (field.type != 2 || !varint(length)) {
            failed_ = true;
            return false;
        }

        if (length > bytes_.size() - position_) {
            failed_ = true;
            return false;
        }

        field.bytes = bytes_.subspan(position_, static_cast<std::size_t>(length));
        position_ += static_cast<std::size_t>(length);
        return true;
    }

    bool done() const {
        return position_ == bytes_.size();
    }

    bool failed() const {
        return failed_;
    }

private:
    bool varint(std::uint64_t& value) {
        value = 0;

        for (unsigned shift = 0; shift < 64 && position_ < bytes_.size(); shift += 7) {
            const auto byte = std::to_integer<unsigned>(bytes_[position_++]);

            if (shift == 63 && byte > 1) {
                return false;
            }

            value |= static_cast<std::uint64_t>(byte & 127) << shift;

            if ((byte & 128) == 0) {
                return true;
            }
        }

        return false;
    }

    std::span<const std::byte> bytes_;
    std::size_t position_ = 0;
    bool failed_ = false;
};

std::wstring wire_string(std::span<const std::byte> bytes) {
    return utf8({reinterpret_cast<const char*>(bytes.data()), bytes.size()});
}

std::filesystem::path environment_path(const wchar_t* name) {
    const DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);

    if (needed == 0 || needed > 32768) {
        return {};
    }

    std::wstring value(needed, L'\0');
    const DWORD copied = GetEnvironmentVariableW(name, value.data(), needed);

    if (copied == 0 || copied >= needed) {
        return {};
    }

    value.resize(copied);
    return value;
}

std::filesystem::path artwork_for(const Product& product, const std::filesystem::path& directory) {
    std::vector<std::filesystem::path> candidates = {
        directory / L"cover.jpg", directory / L"cover.png", directory / L"folder.jpg", directory / L"folder.png"};

    const auto local = environment_path(L"LOCALAPPDATA");

    if (!local.empty()) {
        candidates.push_back(local / L"BetterNet" / L"artwork" / (std::wstring(product.id) + L".jpg"));
        candidates.push_back(local / L"BetterNet" / L"artwork" / (std::wstring(product.id) + L".png"));
    }

    for (const auto& candidate : candidates) {
        std::error_code error;
        const auto size = std::filesystem::file_size(candidate, error);

        if (!error && size > 0 && size <= 16 * 1024 * 1024 && regular_file(candidate)) {
            return candidate;
        }
    }

    return {};
}

using RegistryKey = std::unique_ptr<std::remove_pointer_t<HKEY>, decltype(&RegCloseKey)>;

std::wstring registry_string(HKEY key, const wchar_t* name) {
    DWORD size = 0;
    DWORD type = 0;

    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &size) != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) ||
        size > 65536 || size % sizeof(wchar_t) != 0) {
        return {};
    }

    std::wstring result(size / sizeof(wchar_t) + 1, L'\0');

    if (RegQueryValueExW(key, name, nullptr, nullptr, reinterpret_cast<BYTE*>(result.data()), &size) != ERROR_SUCCESS) {
        return {};
    }

    result.resize(std::wcslen(result.c_str()));

    if (type == REG_EXPAND_SZ) {
        const DWORD needed = ExpandEnvironmentStringsW(result.c_str(), nullptr, 0);

        if (needed == 0 || needed > 32768) {
            return {};
        }

        std::wstring expanded(needed, L'\0');

        if (ExpandEnvironmentStringsW(result.c_str(), expanded.data(), needed) != needed) {
            return {};
        }

        expanded.resize(needed - 1);
        return expanded;
    }

    return result;
}

std::wstring uninstall_product(std::wstring_view command) {
    const auto normalized = lower(command);

    if (normalized.find(L"battle.net") == std::wstring::npos) {
        return {};
    }

    const auto marker = normalized.find(L"--uid=");

    if (marker == std::wstring::npos) {
        return {};
    }

    auto start = marker + 6;

    if (start < normalized.size() && normalized[start] == L'"') {
        ++start;
    }

    const auto end = normalized.find_first_of(L" \t\r\n\"", start);
    auto id = normalized.substr(start, end == std::wstring::npos ? end : end - start);
    return id.size() <= 80 ? id : std::wstring{};
}

void registry_records(HKEY root,
                      REGSAM view,
                      std::vector<InstallationRecord>& records,
                      std::filesystem::path& client,
                      Catalog& status,
                      std::stop_token cancellation) {
    HKEY raw = nullptr;

    const auto opened = RegOpenKeyExW(root, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall", 0, KEY_READ | view, &raw);

    if (opened == ERROR_FILE_NOT_FOUND || opened == ERROR_PATH_NOT_FOUND) {
        return;
    }

    if (opened != ERROR_SUCCESS) {
        status.scan_failed = true;
        status.warnings.push_back(L"An uninstall registry location could not be read.");
        return;
    }

    const RegistryKey parent(raw, RegCloseKey);

    for (DWORD index = 0; index < 32768 && !cancellation.stop_requested(); ++index) {
        std::array<wchar_t, 256> name{};
        DWORD size = static_cast<DWORD>(name.size());
        const auto result = RegEnumKeyExW(parent.get(), index, name.data(), &size, nullptr, nullptr, nullptr, nullptr);

        if (result == ERROR_NO_MORE_ITEMS) {
            return;
        }

        if (result != ERROR_SUCCESS) {
            status.scan_failed = true;
            continue;
        }

        HKEY child_raw = nullptr;

        if (RegOpenKeyExW(parent.get(), name.data(), 0, KEY_READ | view, &child_raw) != ERROR_SUCCESS) {
            status.scan_failed = true;
            continue;
        }

        const RegistryKey child(child_raw, RegCloseKey);
        const auto location = registry_string(child.get(), L"InstallLocation");

        if (location.empty()) {
            continue;
        }

        const auto display = registry_string(child.get(), L"DisplayName");
        const auto command = registry_string(child.get(), L"UninstallString");
        const auto id = uninstall_product(command);

        if ((id == L"battle.net" || lower(display) == L"battle.net") &&
            executable_file(std::filesystem::path(location) / L"Battle.net.exe")) {
            client = std::filesystem::path(location) / L"Battle.net.exe";
        } else if (!id.empty() && records.size() < max_records) {
            records.push_back({id, location});
        }
    }
}
}

std::optional<std::wstring> launch_code_for(std::wstring_view product_id) {
    const auto* product = product_for(product_id);

    if (!product) {
        return std::nullopt;
    }

    return std::wstring(product->launch_code);
}

std::optional<std::wstring_view> direct_launch_arguments_for(std::wstring_view product_id) {
    const auto* product = product_for(product_id);
    return product ? product->direct_arguments : std::nullopt;
}

std::wstring read_build_version(const std::filesystem::path& manifest, std::wstring_view product_id) {
    const auto contents = read_bounded(manifest, max_manifest_bytes);

    if (!contents || contents->find('\0') != std::string::npos) {
        return {};
    }

    const auto lines = split(*contents, '\n', max_manifest_bytes + 1);

    if (lines.size() < 2) {
        return {};
    }

    const auto headers = split(lines.front(), '|');
    const auto missing = std::numeric_limits<std::size_t>::max();
    std::size_t version_index = missing;
    std::size_t active_index = missing;
    std::size_t product_index = missing;

    for (std::size_t index = 0; index < headers.size(); ++index) {
        const auto name = headers[index].substr(0, headers[index].find('!'));

        if (name == "Version") {
            version_index = index;
        } else if (name == "Active") {
            active_index = index;
        } else if (name == "Product") {
            product_index = index;
        }
    }

    if (version_index == missing || active_index == missing) {
        return {};
    }

    for (std::size_t index = 1; index < lines.size(); ++index) {
        auto line = lines[index];

        if (line.ends_with('\r')) {
            line.remove_suffix(1);
        }

        const auto fields = split(line, '|');

        if (fields.size() != headers.size() || fields[active_index] != "1" || !valid_version(fields[version_index])) {
            continue;
        }

        if (product_index != missing && lower(utf8(fields[product_index])) != lower(product_id)) {
            continue;
        }

        return utf8(fields[version_index]);
    }

    return {};
}

AgentRecords parse_agent_records(std::span<const std::byte> data) {
    AgentRecords result;

    if (data.size() > max_database_bytes) {
        result.malformed = true;
        return result;
    }

    WireReader database(data);
    WireField entry;
    std::size_t count = 0;

    while (!database.done() && database.next(entry)) {
        if (++count > max_records) {
            result.malformed = true;
            break;
        }

        if (entry.number != 1 || entry.type != 2) {
            continue;
        }

        WireReader record(entry.bytes);
        WireField field;
        InstallationRecord install;
        bool malformed = false;

        while (!record.done() && record.next(field)) {
            if (field.type != 2) {
                continue;
            }

            if (field.number == 1) {
                install.product_id = wire_string(field.bytes);
            } else if (field.number == 3) {
                WireReader details(field.bytes);
                WireField detail;

                while (!details.done() && details.next(detail)) {
                    if (detail.number == 1 && detail.type == 2) {
                        install.install_path = wire_string(detail.bytes);
                    }
                }

                malformed = malformed || details.failed();
            }
        }

        if (record.failed() || malformed || install.product_id.empty() || install.product_id.size() > 80 ||
            !install.install_path.is_absolute()) {
            result.malformed = true;
            continue;
        }

        result.records.push_back(std::move(install));
    }

    result.malformed = result.malformed || database.failed();
    return result;
}

bool validate_game(const Game& game) {
    const auto* product = product_for(game.product_id);
    std::error_code error;

    if (!product || game.launch_code != product->launch_code || !game.install_path.is_absolute() ||
        !std::filesystem::is_directory(game.install_path, error)) {
        return false;
    }

    const auto executable = game_executable(*product, game.install_path);
    return !executable.empty() && same_path(executable, game.executable);
}

Catalog
catalog_from_records(std::span<const InstallationRecord> records, const std::filesystem::path& client_path, std::stop_token cancellation) {
    Catalog result;
    result.client_path = client_path;

    for (const auto& record : records.first((std::min)(records.size(), max_records))) {
        if (cancellation.stop_requested()) {
            return result;
        }

        const auto* product = product_for(record.product_id);

        if (!product) {
            if (record.product_id != L"agent" && record.product_id != L"battle.net") {
                result.warnings.push_back(L"No verified launch route for installed product: " + record.product_id);
            }

            continue;
        }

        if (!record.install_path.is_absolute()) {
            result.warnings.push_back(L"Invalid installation path for " + std::wstring(product->title));
            continue;
        }

        const auto directory = normalized_path(record.install_path);
        const auto executable = game_executable(*product, directory);

        if (executable.empty()) {
            result.warnings.push_back(L"Installation is missing or incomplete: " + std::wstring(product->title));
            continue;
        }

        const bool duplicate = std::ranges::any_of(result.games, [&](const Game& game) {
            return game.product_id == product->id && same_path(game.install_path, directory);
        });

        if (duplicate) {
            continue;
        }

        auto version = read_build_version(directory / L".build.info", product->id);

        if (version.empty()) {
            version = read_build_version(executable.parent_path() / L".build.info", product->id);
        }

        if (version.empty()) {
            version = executable_version(executable);
        }

        result.games.push_back({std::wstring(product->id),
                                std::wstring(product->title),
                                std::move(version),
                                directory,
                                executable,
                                artwork_for(*product, directory),
                                std::wstring(product->launch_code)});
    }

    std::ranges::sort(result.games, {}, &Game::title);
    std::ranges::sort(result.warnings);
    const auto duplicate_warnings = std::ranges::unique(result.warnings);
    result.warnings.erase(duplicate_warnings.begin(), duplicate_warnings.end());
    return result;
}

Catalog discover(std::stop_token cancellation) {
    Catalog status;
    std::vector<InstallationRecord> records;

    try {
        for (const auto root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
            for (const REGSAM view : {KEY_WOW64_32KEY, KEY_WOW64_64KEY}) {
                registry_records(root, view, records, status.client_path, status, cancellation);
            }
        }

        const auto program_data = environment_path(L"ProgramData");
        std::error_code program_data_error;

        const bool valid_program_data = program_data.is_absolute() && std::filesystem::is_directory(program_data, program_data_error);

        if (!valid_program_data && !cancellation.stop_requested()) {
            status.scan_failed = true;

            status.warnings.push_back(L"Windows ProgramData is unavailable. Battle.net Agent records could not be scanned.");
        }

        if (valid_program_data && !cancellation.stop_requested()) {
            const auto path = program_data / L"Battle.net" / L"Agent" / L"product.db";
            std::error_code error;
            const bool exists = std::filesystem::exists(path, error);

            if (exists) {
                const auto bytes = read_bounded(path, max_database_bytes);

                if (bytes) {
                    const auto parsed = parse_agent_records(std::as_bytes(std::span(*bytes)));
                    records.insert(records.end(), parsed.records.begin(), parsed.records.end());

                    if (parsed.malformed) {
                        status.scan_failed = true;
                        status.warnings.push_back(L"Some Battle.net Agent records were corrupt and were skipped.");
                    }
                } else {
                    status.scan_failed = true;

                    status.warnings.push_back(L"Battle.net Agent records could not be read. Press F5 to rescan after updates finish.");
                }
            } else if (error) {
                status.scan_failed = true;
                status.warnings.push_back(L"Battle.net Agent records could not be accessed.");
            }
        }

        if (status.client_path.empty()) {
            for (const auto& record : records) {
                if (record.product_id == L"battle.net" && executable_file(record.install_path / L"Battle.net.exe")) {
                    status.client_path = record.install_path / L"Battle.net.exe";
                    break;
                }
            }
        }

        if (status.client_path.empty()) {
            for (const auto variable : {L"ProgramFiles(x86)", L"ProgramFiles"}) {
                const auto root = environment_path(variable);

                if (!root.empty() && executable_file(root / L"Battle.net" / L"Battle.net.exe")) {
                    status.client_path = root / L"Battle.net" / L"Battle.net.exe";
                    break;
                }
            }
        }

        auto result = catalog_from_records(records, status.client_path, cancellation);
        result.scan_failed = status.scan_failed;
        result.warnings.insert(result.warnings.end(), status.warnings.begin(), status.warnings.end());
        return result;
    } catch (const std::exception&) {
        status.scan_failed = true;
        status.warnings.push_back(L"Discovery could not finish. Check installation access and press F5 to rescan.");
        return status;
    }
}
}

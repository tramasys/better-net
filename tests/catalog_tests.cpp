#include "catalog.hpp"
#include <Windows.h>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
void require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

struct Fixture
{
    std::filesystem::path path = std::filesystem::temp_directory_path() /
                                 (L"Bnet catalog tests \u03b4 \u65e5\u672c " + std::to_wstring(GetCurrentProcessId()));

    Fixture()
    {
        require(!std::filesystem::exists(path), "Fixture path must be new");
        std::filesystem::create_directories(path);
    }

    ~Fixture()
    {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

void write(const std::filesystem::path& path, std::string_view contents)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    require(static_cast<bool>(stream), "Fixture write failed");
}

void executable(const std::filesystem::path& path)
{
    std::string bytes(128, '\0');
    bytes[0] = 'M';
    bytes[1] = 'Z';
    bytes[60] = 64;
    bytes[64] = 'P';
    bytes[65] = 'E';
    bytes[86] = 2;
    write(path, bytes);
}

std::string manifest(std::string_view id, std::string_view version, std::string_view active = "1")
{
    return "Branch!STRING:0|Active!DEC:1|Version!STRING:0|Product!STRING:0\r\n" + std::string("eu|") +
           std::string(active) + "|" + std::string(version) + "|" + std::string(id) + "\r\n";
}

void varint(std::string& output, std::uint64_t value)
{
    while (value >= 128)
    {
        output.push_back(static_cast<char>((value & 127) | 128));
        value >>= 7;
    }

    output.push_back(static_cast<char>(value));
}

std::string field(unsigned number, std::string_view content)
{
    std::string output;
    varint(output, static_cast<std::uint64_t>(number) * 8 + 2);
    varint(output, content.size());
    output.append(content);
    return output;
}

std::string utf8(const std::filesystem::path& path)
{
    const auto bytes = path.u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

bnet::AgentRecords parse(std::string_view bytes)
{
    return bnet::parse_agent_records(std::as_bytes(std::span(bytes.data(), bytes.size())));
}

void test_catalog(const Fixture& fixture)
{
    const auto retail = fixture.path / L"Call of Duty";
    const auto other_install = fixture.path / L"Other Call of Duty";
    executable(retail / L"_retail_" / L"cod.exe");
    executable(other_install / L"_retail_" / L"cod.exe");
    executable(retail / L"_beta_" / L"cod26-cod.exe");
    write(retail / L".build.info", manifest("auks", "3.14.2.28787707"));
    write(other_install / L".build.info", manifest("auks", "3.12.1.1234"));

    const std::array records = {bnet::InstallationRecord{L"auks", retail, L"An obsolete component title"},
                                bnet::InstallationRecord{L"AUKS", retail / L"_retail_" / L"..", L"Duplicate"},
                                bnet::InstallationRecord{L"auks", other_install, L"Second installation"},
                                bnet::InstallationRecord{L"auks", fixture.path / L"Missing", L"Stale"},
                                bnet::InstallationRecord{L"auks_beta", retail, L"Beta"},
                                bnet::InstallationRecord{L"made-up-id", retail, L"Untrusted"}};

    const auto catalog = bnet::catalog_from_records(records);
    require(catalog.games.size() == 2, "Deduplicate identity and path while retaining distinct installations");
    require(catalog.games[0].title == L"Call of Duty", "Use maintained title, not component or registry title");
    require(catalog.games[0].version == L"3.14.2.28787707", "Read matching active product build");
    require(catalog.games[0].launch_code == L"AUKS", "Use verified launch code");
    require(!catalog.warnings.empty(), "Unsupported and stale records need diagnostics");
    require(bnet::validate_game(catalog.games[0]), "Unicode spaced installation must validate");
    auto changed = catalog.games[0];
    changed.launch_code = L"ODIN";
    require(!bnet::validate_game(changed), "Cross-product descriptor must be rejected");
    changed = catalog.games[0];
    changed.executable = retail / L"_beta_" / L"cod26-cod.exe";
    require(!bnet::validate_game(changed), "Wrong channel executable must be rejected");
    std::filesystem::remove(retail / L"_retail_" / L"cod.exe");
    require(!bnet::validate_game(catalog.games[0]), "Revalidation must detect removed executable");
    std::stop_source cancellation;
    cancellation.request_stop();
    require(bnet::catalog_from_records(records, {}, cancellation.get_token()).games.empty(), "Honor cancellation");
    require(!bnet::launch_code_for(L"auks_beta"), "Unknown beta route must not alias retail");
    require(!bnet::launch_code_for(L"AUKS --attack"), "Reject malformed product identifiers");
}

void test_manifests(const Fixture& fixture)
{
    const auto path = fixture.path / L"metadata" / L".build.info";
    write(path, manifest("auks_beta", "99.99"));
    require(bnet::read_build_version(path, L"auks").empty(), "Do not borrow another channel version");
    write(path, manifest("auks", "1.2.3", "0"));
    require(bnet::read_build_version(path, L"auks").empty(), "Ignore inactive version rows");
    write(path, manifest("auks", "not a version"));
    require(bnet::read_build_version(path, L"auks").empty(), "Reject malformed versions");
    write(path, "Active!DEC:1|Version!STRING:0|Product!STRING:0\n1|8.9|auks\n1|truncated\n");
    require(bnet::read_build_version(path, L"auks") == L"8.9", "Malformed later rows must preserve valid metadata");
    write(path, "Active!DEC:1|Version!STRING:0|Product!STRING:0\n1|truncated\n1|8.9|auks\n");
    require(bnet::read_build_version(path, L"auks") == L"8.9", "Malformed earlier rows must not hide valid metadata");
    std::string many_rows = "Active!DEC:1|Version!STRING:0|Product!STRING:0\n";

    for (std::size_t index = 0; index < 128; ++index)
    {
        many_rows += "malformed\n";
    }

    many_rows += "1|8.9|auks\n";
    write(path, many_rows);
    require(bnet::read_build_version(path, L"auks") == L"8.9", "Long malformed prefix must not hide valid metadata");
    std::string wide_header = "Active!DEC:1|Version!STRING:0";
    std::string wide_record = "1|88.99";

    for (std::size_t index = 0; index < 64; ++index)
    {
        wide_header += "|Extra!STRING:0";
        wide_record += "|x";
    }

    write(path, wide_header + "|Product!STRING:0\n" + wide_record + "|other_channel\n");

    require(bnet::read_build_version(path, L"auks").empty(),
            "Reject over-wide fields rather than lose product identity");

    write(path, std::string(1024 * 1024 + 1, 'x'));
    require(bnet::read_build_version(path, L"auks").empty(), "Manifest reads must be bounded");
    write(path, std::string("Version!STRING:0\0garbage", 24));
    require(bnet::read_build_version(path, L"auks").empty(), "Reject embedded NUL metadata");

    require(bnet::read_build_version(fixture.path / L"missing.info", L"auks").empty(),
            "Missing manifest is recoverable");
}

void test_database(const Fixture& fixture)
{
    const auto directory = fixture.path / L"Database game";
    const auto valid = field(1, field(1, "auks") + field(2, "auks") + field(3, field(1, utf8(directory))));
    const auto initial = parse(valid);
    require(initial.records.size() == 1 && !initial.malformed, "Parse protobuf Agent install record");
    require(initial.records[0].install_path == directory, "Preserve UTF-8 Windows installation path");
    const auto malformed = field(1, std::string("\x0a\xff", 2));
    const auto recoverable = parse(malformed + valid + malformed + valid);
    require(recoverable.malformed && recoverable.records.size() == 2, "Bad entry must not hide sibling entries");
    const auto truncated = parse(valid + std::string("\x0a\x7f", 2));
    require(truncated.malformed && truncated.records.size() == 1, "Truncated tail retains complete records");
    const auto relative = parse(field(1, field(1, "auks") + field(3, field(1, "relative/path"))));
    require(relative.malformed && relative.records.empty(), "Reject relative installation paths");
    const auto invalid_utf8 = parse(field(1, field(1, "auks") + field(3, field(1, "C:/bad\xff"))));
    require(invalid_utf8.malformed && invalid_utf8.records.empty(), "Reject invalid UTF-8 paths");
    const auto oversized = parse(std::string(4 * 1024 * 1024 + 1, '\0'));
    require(oversized.malformed && oversized.records.empty(), "Database reads must be bounded");
    const auto varint_overflow = parse(std::string(10, '\xff'));
    require(varint_overflow.malformed, "Reject overflowing protobuf varints");
    const auto empty = parse({});
    require(empty.records.empty() && !empty.malformed, "Empty database is distinct from corrupt database");
}

void test_fallback(const Fixture& fixture)
{
    const auto directory = fixture.path / L"Version fallback";
    const auto path = directory / L"_retail_" / L"Overwatch.exe";
    executable(path);
    write(directory / L"Battle.net Launcher.exe", "Pretend launcher version 99.99");
    const std::array records = {bnet::InstallationRecord{L"prometheus", directory, L"Overwatch"}};
    auto result = bnet::catalog_from_records(records);
    require(result.games.size() == 1 && result.games[0].version.empty(), "Absent game version must remain unknown");
    write(directory / L".build.info", "corrupt manifest");
    write(directory / L"_retail_" / L".build.info", manifest("prometheus", "2.4.6"));
    result = bnet::catalog_from_records(records);
    require(result.games[0].version == L"2.4.6", "Fall back to product executable directory manifest");
    std::filesystem::remove(directory / L"_retail_" / L".build.info");
    std::array<wchar_t, MAX_PATH> windows{};
    require(GetWindowsDirectoryW(windows.data(), static_cast<UINT>(windows.size())) != 0, "Get Windows fixture source");
    const auto source = std::filesystem::path(windows.data()) / L"System32" / L"notepad.exe";
    require(CopyFileW(source.c_str(), path.c_str(), FALSE) != FALSE, "Copy versioned executable into fixture");
    result = bnet::catalog_from_records(records);

    require(result.games.size() == 1 && !result.games[0].version.empty(),
            "Fall back to actual game executable metadata");

    write(path, "not an executable");
    require(bnet::catalog_from_records(records).games.empty(), "Corrupt executable must not be admitted");
}

void test_versioned_executable(const Fixture& fixture)
{
    const auto directory = fixture.path / L"StarCraft II";
    executable(directory / L"StarCraft II.exe");
    const std::array records = {bnet::InstallationRecord{L"s2", directory, L"StarCraft II"}};
    require(bnet::catalog_from_records(records).games.empty(), "A launcher alone is not an installed game");
    executable(directory / L"Versions" / L"Base9" / L"SC2_x64.exe");
    executable(directory / L"Versions" / L"Base100" / L"SC2_x64.exe");
    executable(directory / L"Versions" / L"Baseoops" / L"SC2_x64.exe");
    write(directory / L"Versions" / L"Base101" / L"SC2_x64.exe", "incomplete update");
    const auto result = bnet::catalog_from_records(records);
    require(result.games.size() == 1, "Discover bounded versioned product directory");
    require(result.games[0].executable.parent_path().filename() == L"Base100", "Choose newest valid numeric build");
    require(result.games[0].version.empty(), "Never turn directory name into fabricated version");
}

int scan()
{
    const auto catalog = bnet::discover();
    std::wcout << L"Client: " << catalog.client_path.wstring() << L'\n';
    std::wcout << L"Games: " << catalog.games.size() << L"; scan_failed: " << catalog.scan_failed << L'\n';

    for (const auto& game : catalog.games)
    {
        std::wcout << game.product_id << L" | " << game.title << L" | " << game.version << L" | " << game.launch_code
                   << L'\n';

        std::wcout << L"  Installation: " << game.install_path.wstring() << L'\n';
        std::wcout << L"  Executable: " << game.executable.wstring() << L'\n';
        std::wcout << L"  Artwork: " << game.artwork_path.wstring() << L'\n';
        std::wcout << L"  Valid: " << bnet::validate_game(game) << L'\n';
    }

    for (const auto& warning : catalog.warnings)
    {
        std::wcout << L"Warning: " << warning << L'\n';
    }

    return catalog.scan_failed ? 1 : 0;
}
}

int main(int argc, char** argv)
{
    try
    {
        if (argc == 2 && std::string_view(argv[1]) == "--scan")
        {
            return scan();
        }

        const Fixture fixture;
        test_catalog(fixture);
        test_manifests(fixture);
        test_database(fixture);
        test_fallback(fixture);
        test_versioned_executable(fixture);
        std::cout << "Catalog fixtures passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

#include "key_store.h"
#include <windows.h>
#include <wincrypt.h>
#include <shlobj.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

namespace KeyStore {

namespace {

// Resolve %APPDATA%\WaveMind\key.dat as a filesystem::path, creating the
// folder if needed. Keeping it as a path (not a narrow string) means fstream
// opens it correctly even when the user profile contains non-ASCII characters.
std::expected<std::filesystem::path, std::string> resolve_key_path() {
    wchar_t* appdata_w = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata_w))) {
        return std::unexpected("Failed to resolve the APPDATA folder");
    }
    std::filesystem::path dir = std::filesystem::path(appdata_w) / L"WaveMind";
    CoTaskMemFree(appdata_w);

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        return std::unexpected("Failed to create directory: " + ec.message());
    }
    return dir / L"key.dat";
}

} // anonymous namespace

std::expected<std::string, std::string> key_file_path() {
    auto path = resolve_key_path();
    if (!path) return std::unexpected(path.error());
    try {
        return path->string();
    } catch (const std::exception& e) {
        return std::unexpected(std::string("Failed to render key path: ") + e.what());
    }
}

std::expected<void, std::string> save(const std::string& plaintext) {
    auto path = resolve_key_path();
    if (!path) return std::unexpected(path.error());

    DATA_BLOB input{};
    input.cbData = static_cast<DWORD>(plaintext.size());
    input.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plaintext.data()));

    DATA_BLOB output{};
    // dwFlags = 0 selects user-scope encryption: only this Windows account can decrypt.
    if (!CryptProtectData(&input, L"WaveMind API Key", nullptr, nullptr, nullptr,
                          0, &output)) {
        return std::unexpected("CryptProtectData failed: " + std::to_string(GetLastError()));
    }

    std::vector<BYTE> encrypted(output.pbData, output.pbData + output.cbData);
    LocalFree(output.pbData);

    std::ofstream ofs(*path, std::ios::binary | std::ios::trunc);
    if (!ofs) {
        return std::unexpected("Failed to open key file for writing");
    }
    ofs.write(reinterpret_cast<const char*>(encrypted.data()),
              static_cast<std::streamsize>(encrypted.size()));
    if (!ofs) {
        return std::unexpected("Failed to write key file");
    }
    return {};
}

std::expected<std::string, std::string> load() {
    auto path = resolve_key_path();
    if (!path) return std::unexpected(path.error());

    std::ifstream ifs(*path, std::ios::binary);
    if (!ifs) {
        return std::unexpected("Key file not found");
    }

    std::vector<BYTE> encrypted((std::istreambuf_iterator<char>(ifs)),
                                 std::istreambuf_iterator<char>());
    if (encrypted.empty()) {
        return std::unexpected("Key file is empty");
    }

    DATA_BLOB input{};
    input.cbData = static_cast<DWORD>(encrypted.size());
    input.pbData = encrypted.data();

    DATA_BLOB output{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, 0, &output)) {
        return std::unexpected("CryptUnprotectData failed: " + std::to_string(GetLastError()));
    }

    std::string plaintext(reinterpret_cast<const char*>(output.pbData), output.cbData);
    LocalFree(output.pbData);
    return plaintext;
}

} // namespace KeyStore

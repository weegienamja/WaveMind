#pragma once
#include <expected>
#include <string>

// Encrypt and persist the API key using Windows DPAPI.
// Stored at %APPDATA%\WaveMind\key.dat (user-scope encryption).
namespace KeyStore {

// Returns the full path to the key file, creating the directory if needed.
// Returns an error string on failure.
std::expected<std::string, std::string> key_file_path();

// Encrypt 'plaintext' with DPAPI and write to key.dat.
std::expected<void, std::string> save(const std::string& plaintext);

// Read key.dat and decrypt with DPAPI.
// Returns the plaintext key, or an error string.
std::expected<std::string, std::string> load();

} // namespace KeyStore

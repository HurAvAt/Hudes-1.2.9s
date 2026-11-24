#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <string>

namespace HudesNative
{
    struct DatabasePasswordRequired : std::runtime_error
    {
        DatabasePasswordRequired() : std::runtime_error("The database password is missing or incorrect, or the file is damaged.") {}
    };

    struct DatabaseMigrationChoiceRequired : std::runtime_error
    {
        DatabaseMigrationChoiceRequired() : std::runtime_error("Choose whether to set a password for the converted database.") {}
    };

    struct DatabaseKey
    {
        std::array<std::uint8_t, 16> salt{};
        std::array<std::uint8_t, 32> bytes{};
        DatabaseKey() = default;
        DatabaseKey(DatabaseKey const&) = delete;
        DatabaseKey& operator=(DatabaseKey const&) = delete;
        ~DatabaseKey();
    };

    struct DecodedDatabase
    {
        std::string json;
        std::unique_ptr<DatabaseKey> key;
        bool legacy{};
        bool windowsBound{};
    };

    std::unique_ptr<DatabaseKey> MakeDatabaseKey(std::wstring_view password);
    DecodedDatabase DecodeDatabaseFile(std::filesystem::path const& path, std::wstring_view password);
    void WriteDatabaseFile(std::filesystem::path const& path, std::string const& json, DatabaseKey const* key);
}

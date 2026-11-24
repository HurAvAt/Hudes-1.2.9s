#include "pch.h"
#include "DatabaseFile.h"
#include <bcrypt.h>
#include <compressapi.h>
#include <wincrypt.h>
#include <cstring>
#include <fstream>

using namespace winrt;
using HudesNative::DatabaseKey;

namespace
{
    constexpr std::string_view compressedHeader = "SALIFDB1C\n";
    constexpr std::string_view passwordHeader = "SALIFDB1P\n";
    constexpr std::string_view legacyDpapiHeader = "SALIF-DPAPI-1\n";
    constexpr std::size_t maxJsonSize = 512 * 1024 * 1024;
    constexpr ULONG nonceSize = 12;
    constexpr ULONG tagSize = 16;
    constexpr ULONGLONG passwordIterations = 600000;

    void FillRandom(PUCHAR buffer, ULONG size)
    {
        check_nt(BCryptGenRandom(nullptr, buffer, size, BCRYPT_USE_SYSTEM_PREFERRED_RNG));
    }

    std::unique_ptr<DatabaseKey> DeriveKey(std::wstring_view password, std::array<std::uint8_t, 16> const& salt)
    {
        auto result = std::make_unique<DatabaseKey>();
        result->salt = salt;
        auto utf8 = to_string(hstring(password));
        auto erasePassword = wil::scope_exit([&] { SecureZeroMemory(utf8.data(), utf8.size()); });
        BCRYPT_ALG_HANDLE algorithm{};
        check_nt(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG));
        auto release = wil::scope_exit([&] { BCryptCloseAlgorithmProvider(algorithm, 0); });
        check_nt(BCryptDeriveKeyPBKDF2(algorithm, reinterpret_cast<PUCHAR>(utf8.data()), static_cast<ULONG>(utf8.size()),
            result->salt.data(), static_cast<ULONG>(result->salt.size()), passwordIterations,
            result->bytes.data(), static_cast<ULONG>(result->bytes.size()), 0));
        return result;
    }

    std::string TransformGcm(std::string_view data, DatabaseKey const& key, std::array<BYTE, nonceSize>& nonce,
        std::array<BYTE, tagSize>& tag, bool encrypt)
    {
        BCRYPT_ALG_HANDLE algorithm{};
        check_nt(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_AES_ALGORITHM, nullptr, 0));
        auto release = wil::scope_exit([&] { BCryptCloseAlgorithmProvider(algorithm, 0); });
        check_nt(BCryptSetProperty(algorithm, BCRYPT_CHAINING_MODE,
            reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)), sizeof(BCRYPT_CHAIN_MODE_GCM), 0));
        BCRYPT_KEY_HANDLE symmetricKey{};
        check_nt(BCryptGenerateSymmetricKey(algorithm, &symmetricKey, nullptr, 0,
            const_cast<PUCHAR>(key.bytes.data()), static_cast<ULONG>(key.bytes.size()), 0));
        auto releaseKey = wil::scope_exit([&] { BCryptDestroyKey(symmetricKey); });
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO auth{};
        BCRYPT_INIT_AUTH_MODE_INFO(auth);
        auth.pbNonce = nonce.data();
        auth.cbNonce = nonceSize;
        auth.pbTag = tag.data();
        auth.cbTag = tagSize;
        auth.pbAuthData = reinterpret_cast<PUCHAR>(const_cast<char*>(passwordHeader.data()));
        auth.cbAuthData = static_cast<ULONG>(passwordHeader.size());

        auto transform = encrypt ? BCryptEncrypt : BCryptDecrypt;
        auto size = static_cast<ULONG>(data.size());
        std::string result(size, '\0');
        ULONG written{};
        auto status = transform(symmetricKey, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())), size,
            &auth, nullptr, 0, reinterpret_cast<PUCHAR>(result.data()), size, &written, 0);
        // STATUS_AUTH_TAG_MISMATCH
        if (!encrypt && status == static_cast<NTSTATUS>(0xC000A002L))
            throw HudesNative::DatabasePasswordRequired();
        check_nt(status);
        result.resize(written);
        return result;
    }

    std::string CompressJson(std::string const& json)
    {
        COMPRESSOR_HANDLE compressor{};
        check_bool(CreateCompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, nullptr, &compressor));
        auto release = wil::scope_exit([&] { CloseCompressor(compressor); });
        SIZE_T size{};
        if (!Compress(compressor, json.data(), json.size(), nullptr, 0, &size) && GetLastError() != ERROR_INSUFFICIENT_BUFFER)
            throw_last_error();
        std::string result(size, '\0');
        check_bool(Compress(compressor, json.data(), json.size(), result.data(), result.size(), &size));
        result.resize(size);
        return result;
    }

    std::string DecompressJson(std::string_view bytes)
    {
        DECOMPRESSOR_HANDLE decompressor{};
        check_bool(CreateDecompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, nullptr, &decompressor));
        auto release = wil::scope_exit([&] { CloseDecompressor(decompressor); });
        SIZE_T size{};
        if (!Decompress(decompressor, bytes.data(), bytes.size(), nullptr, 0, &size) && GetLastError() != ERROR_INSUFFICIENT_BUFFER)
            throw_last_error();
        if (!size || size > maxJsonSize)
            throw hresult_error(E_INVALIDARG, L"The database size is invalid.");
        std::string result(size, '\0');
        check_bool(Decompress(decompressor, bytes.data(), bytes.size(), result.data(), result.size(), &size));
        result.resize(size);
        return result;
    }

    std::string DecryptLegacy(std::string_view bytes)
    {
        DATA_BLOB input{ static_cast<DWORD>(bytes.size()), reinterpret_cast<BYTE*>(const_cast<char*>(bytes.data())) };
        DATA_BLOB output{};
        if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output))
            throw hresult_error(E_FAIL, L"Open this older backup on its original PC and Windows account once to convert it to Hudes's portable format.");
        auto release = wil::scope_exit([&] { SecureZeroMemory(output.pbData, output.cbData); LocalFree(output.pbData); });
        return std::string(reinterpret_cast<char const*>(output.pbData), output.cbData);
    }

    void WriteAtomically(std::filesystem::path const& path, std::string const& contents)
    {
        GUID id{};
        check_hresult(CoCreateGuid(&id));
        wchar_t suffix[40]{};
        StringFromGUID2(id, suffix, 40);
        auto temporary = path;
        temporary += std::wstring(L".") + suffix + L".tmp";
        auto cleanup = wil::scope_exit([&] { std::error_code ignored; std::filesystem::remove(temporary, ignored); });
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("The database could not be written.");
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        output.close();
        if (!output) throw std::runtime_error("The database write did not complete.");
        check_bool(MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH));
        cleanup.release();
    }
}

namespace HudesNative
{
    DatabaseKey::~DatabaseKey()
    {
        SecureZeroMemory(bytes.data(), bytes.size());
    }

    std::unique_ptr<DatabaseKey> MakeDatabaseKey(std::wstring_view password)
    {
        if (password.empty()) return {};
        std::array<std::uint8_t, 16> salt{};
        FillRandom(salt.data(), static_cast<ULONG>(salt.size()));
        return DeriveKey(password, salt);
    }

    DecodedDatabase DecodeDatabaseFile(std::filesystem::path const& path, std::wstring_view password)
    {
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input) throw std::runtime_error("The database could not be opened.");
        auto size = static_cast<std::streamoff>(input.tellg());
        if (size <= 0 || static_cast<std::size_t>(size) > maxJsonSize + maxJsonSize / 8)
            throw hresult_error(E_INVALIDARG, L"The database file size is invalid.");
        std::string contents(static_cast<std::size_t>(size), '\0');
        input.seekg(0);
        if (!input.read(contents.data(), static_cast<std::streamsize>(contents.size())))
            throw std::runtime_error("The database could not be read completely.");

        std::string_view bytes(contents);
        if (bytes.starts_with(compressedHeader)) return { DecompressJson(bytes.substr(compressedHeader.size())), nullptr, false };
        if (bytes.starts_with(passwordHeader))
        {
            std::array<std::uint8_t, 16> salt{};
            std::array<BYTE, nonceSize> nonce{};
            std::array<BYTE, tagSize> tag{};
            bytes.remove_prefix(passwordHeader.size());
            if (bytes.size() <= salt.size() + nonce.size() + tag.size())
                throw hresult_error(E_INVALIDARG, L"The password-protected database is incomplete.");
            if (password.empty()) throw DatabasePasswordRequired();
            std::memcpy(salt.data(), bytes.data(), salt.size());
            bytes.remove_prefix(salt.size());
            std::memcpy(nonce.data(), bytes.data(), nonce.size());
            bytes.remove_prefix(nonce.size());
            std::memcpy(tag.data(), bytes.data(), tag.size());
            bytes.remove_prefix(tag.size());
            auto key = DeriveKey(password, salt);
            auto json = TransformGcm(bytes, *key, nonce, tag, false);
            return { std::move(json), std::move(key), false };
        }
        if (bytes.starts_with(legacyDpapiHeader))
            return { DecryptLegacy(bytes.substr(legacyDpapiHeader.size())), nullptr, true, true };
        return { std::move(contents), nullptr, true };
    }

    void WriteDatabaseFile(std::filesystem::path const& path, std::string const& json, DatabaseKey const* key)
    {
        if (json.size() > maxJsonSize)
            throw hresult_error(E_INVALIDARG, L"The database is too large to save.");
        if (!key)
        {
            WriteAtomically(path, std::string(compressedHeader) + CompressJson(json));
            return;
        }
        std::array<BYTE, nonceSize> nonce{};
        std::array<BYTE, tagSize> tag{};
        FillRandom(nonce.data(), nonceSize);
        auto encrypted = TransformGcm(json, *key, nonce, tag, true);
        std::string contents(passwordHeader);
        contents.append(reinterpret_cast<char const*>(key->salt.data()), key->salt.size());
        contents.append(reinterpret_cast<char const*>(nonce.data()), nonce.size());
        contents.append(reinterpret_cast<char const*>(tag.data()), tag.size());
        contents.append(encrypted);
        WriteAtomically(path, contents);
    }
}

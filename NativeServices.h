#pragma once

#include <string_view>

#include "Models.h"
#include <functional>
#include <span>
#include <stop_token>
#include <thread>

namespace HudesNative
{
    struct unique_handle
    {
        unique_handle() noexcept = default;
        explicit unique_handle(HANDLE value) noexcept : m_value(value) {}
        ~unique_handle() { reset(); }
        unique_handle(unique_handle const&) = delete;
        unique_handle& operator=(unique_handle const&) = delete;
        unique_handle(unique_handle&& other) noexcept : m_value(std::exchange(other.m_value, nullptr)) {}
        unique_handle& operator=(unique_handle&& other) noexcept
        {
            if (this != &other) reset(std::exchange(other.m_value, nullptr));
            return *this;
        }
        HANDLE get() const noexcept { return m_value; }
        explicit operator bool() const noexcept { return m_value && m_value != INVALID_HANDLE_VALUE; }
        void reset(HANDLE value = nullptr) noexcept
        {
            if (*this) CloseHandle(m_value);
            m_value = value;
        }
    private:
        HANDLE m_value{};
    };

    class DirectoryWatcher final
    {
    public:
        DirectoryWatcher() = default;
        ~DirectoryWatcher();
        DirectoryWatcher(DirectoryWatcher const&) = delete;
        DirectoryWatcher& operator=(DirectoryWatcher const&) = delete;

        void Start(std::filesystem::path const& folder, std::function<void()> callback);
        void Stop() noexcept;

    private:
        std::jthread m_thread;
        unique_handle m_stopEvent;
    };

    std::filesystem::path ExecutableFolder();
    bool SamePath(std::filesystem::path const& left, std::filesystem::path const& right) noexcept;
    bool IsWithin(std::filesystem::path const& path, std::filesystem::path const& folder) noexcept;
    std::filesystem::path UniqueDestination(std::filesystem::path const& path);
    void ValidateItemName(std::wstring_view name);
    std::filesystem::path CreateEmptyItem(std::filesystem::path const& folder, std::wstring const& name, bool directory);
    std::wstring FriendlyError(std::exception_ptr error = std::current_exception());
    std::wstring FormatSize(std::uintmax_t size);
    std::wstring FileTimeText(std::filesystem::file_time_type value);
    bool FileNameMatches(std::wstring const& fileName, std::wstring const& query);
    bool ReminderIsNear(std::optional<std::wstring> const& value, double warningHours);
    std::wstring LocalDateTimeText(SYSTEMTIME const& value);

    std::optional<std::filesystem::path> PickFolder(HWND owner, std::wstring const& title,
        std::filesystem::path const& initial = {});
    std::optional<std::filesystem::path> PickOpenFile(HWND owner, std::wstring const& title,
        std::wstring const& filterName, std::wstring const& extension);
    std::optional<std::filesystem::path> PickSaveFile(HWND owner, std::wstring const& title,
        std::wstring const& filterName, std::wstring const& extension, std::wstring const& suggestedName);

    void OpenPath(std::filesystem::path const& path);
    void ShowOpenWith(HWND owner, std::filesystem::path const& path);
    void ShowInExplorer(std::filesystem::path const& path);
    std::filesystem::path CopyToFolder(std::filesystem::path const& source, std::filesystem::path const& folder, bool move);
    void MoveTo(std::filesystem::path const& source, std::filesystem::path const& destination);
    void DeleteToRecycleBin(HWND owner, std::span<std::filesystem::path const> paths);
    void SetClipboardFiles(HWND owner, std::span<std::filesystem::path const> paths, bool cut);
    std::vector<std::filesystem::path> GetClipboardFiles(bool& move);
    std::filesystem::path ImportShortcutLogo(std::filesystem::path const& app, std::filesystem::path source);
    enum class FileHashAlgorithm { Md5, Sha256 };
    std::wstring CalculateFileHash(std::filesystem::path const& path, FileHashAlgorithm algorithm);

    std::vector<std::filesystem::path> CreateZip(std::span<std::filesystem::path const> sources,
        std::filesystem::path const& outputZip);
    void ExtractZip(std::filesystem::path const& archivePath, std::filesystem::path const& destination);

    std::vector<MetadataEntry> ReadMetadata(std::filesystem::path const& path);
    std::vector<std::wstring> SaveMetadata(std::filesystem::path const& path, std::span<MetadataEntry> entries);
}

#pragma once

#include "Models.h"
#include "DatabaseFile.h"
#include <mutex>
#include <span>

namespace HudesNative
{
    bool IsAutomaticTag(std::wstring const& tag) noexcept;

    class AppStore final
    {
    public:
        explicit AppStore(std::filesystem::path databasePath = {}, std::wstring_view databasePassword = {},
            std::optional<std::wstring_view> migrationPassword = std::nullopt);

        AppStore(AppStore const&) = delete;
        AppStore& operator=(AppStore const&) = delete;

        std::filesystem::path const& DatabasePath() const noexcept;
        AppSettings& Settings() noexcept;
        AppSettings const& Settings() const noexcept;
        std::vector<FileGroup> const& Groups() const noexcept;
        std::vector<std::wstring> const& Tags() const noexcept;
        std::vector<FileNote> const& Notes() const noexcept;
        std::filesystem::path const& LegacyArchiveRoot() const noexcept;
        std::vector<std::filesystem::path> const& LegacyArchiveGroups() const noexcept;
        bool IsArchived(std::filesystem::path const& path) const noexcept;
        void SetArchived(std::span<std::filesystem::path const> paths, bool archived);
        std::vector<PanelNote> const& PanelNotes() const noexcept;
        void SavePanelNote(PanelNote note);
        void DeletePanelNote(std::wstring const& id);
        void SetPinned(std::span<std::filesystem::path const> paths, bool pinned);

        FileNote* Get(std::filesystem::path const& path) noexcept;
        FileNote const* Get(std::filesystem::path const& path) const noexcept;
        FileGroup const* GetGroup(std::optional<std::wstring> const& id) const noexcept;
        std::vector<FileNote const*> Search(std::wstring const& query) const;

        void SaveNote(std::filesystem::path const& path, std::wstring description,
            std::vector<std::wstring> tags, std::optional<std::wstring> groupId);
        void SetGroup(std::span<std::filesystem::path const> paths, std::optional<std::wstring> groupId);
        void SetStatus(std::span<std::filesystem::path const> paths, std::wstring const& status);
        void SetReminder(std::span<std::filesystem::path const> paths, std::optional<std::wstring> reminderAt);
        FileGroup const& AddGroup(std::wstring name, std::wstring color);
        void UpdateGroup(std::wstring const& id, std::wstring name, std::wstring color);
        std::wstring const& AddTag(std::wstring name);
        std::wstring RenameTag(std::wstring const& oldName, std::wstring name);
        void ReorderTag(std::wstring const& name, std::wstring const& target);
        bool GroupInUse(std::wstring const& id) const noexcept;
        bool DeleteGroup(std::wstring const& id);
        bool DeleteTag(std::wstring const& name);
        void MovePath(std::filesystem::path const& oldPath, std::filesystem::path const& newPath);
        void MovePaths(std::span<std::pair<std::filesystem::path, std::filesystem::path> const> paths);
        void DeletePath(std::filesystem::path const& path);
        void DeletePaths(std::span<std::filesystem::path const> paths);
        void ReplaceWithArchive(std::span<std::filesystem::path const> sourcePaths,
            std::filesystem::path const& archivePath, std::optional<std::wstring> groupId);

        void Save();
        void BackupTo(std::filesystem::path const& path, std::wstring_view backupPassword = {}) const;
        void RestoreFrom(std::filesystem::path const& path, std::wstring_view backupPassword = {});
        bool IsDatabaseProtected() const noexcept;
        void SetDatabasePassword(std::wstring_view password);
        bool VerifyPassword(std::wstring const& password) const;
        void SetPassword(std::wstring const& password);
        bool GroupInherited(std::filesystem::path const& path) const;
        void InheritNewChildren(std::filesystem::path const& folder);
        void RegisterArchiveGroup(std::filesystem::path const& folder, std::wstring const& groupId);

    private:
        std::filesystem::path m_databasePath;
        Database m_database;
        std::unique_ptr<DatabaseKey> m_databaseKey;
        mutable std::recursive_mutex m_mutex;

        void LoadOrCreate(std::wstring_view password, std::optional<std::wstring_view> migrationPassword);
        void EnsureDefaults();
        void ImportLegacyArchive();
        void PersistLocked() const;
        FileNote& GetOrCreateLocked(std::filesystem::path const& path);
        static void MarkStarted(FileNote& note);
        bool Reconcile(std::filesystem::path const& path, bool includeRoot, bool recursive);
    };
}

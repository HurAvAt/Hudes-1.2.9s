#pragma once

#include "Models.h"
#include <span>

namespace HudesNative
{
    std::filesystem::path const& ArchiveRoot();
    std::filesystem::path ArchiveGroupPath(std::optional<std::wstring> const& groupId);
    bool HiddenInFileView(std::filesystem::path path, std::filesystem::path const& stopAt = {});
    bool IsVirtualArchive(std::filesystem::path const& path);
    std::wstring NextFileName(std::filesystem::path const& folder, std::wstring const& base, std::wstring const& extension);

    struct WorkspaceExportResult
    {
        std::filesystem::path folder;
        std::size_t records{};
        std::size_t parts{};
        std::size_t skipped{};
    };

    WorkspaceExportResult ExportWorkspace(std::filesystem::path const& workspace,
        std::filesystem::path const& legacyRoot, std::filesystem::path const& destination,
        std::span<FileGroup const> groups, std::span<FileNote const> notes,
        bool xlsx, std::uint32_t recordLimit, std::filesystem::path const& virtualSelection = {});
}

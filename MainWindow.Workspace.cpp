#include "pch.h"
#include "MainWindow.xaml.h"
#include "Workspace.h"
#include <algorithm>
#include <sstream>
#include <utility>

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
namespace fs = std::filesystem;

namespace
{
    std::wstring TrimSetting(std::wstring value)
    {
        auto start = value.find_first_not_of(L" \t\r\n");
        if (start == value.npos) return {};
        return value.substr(start, value.find_last_not_of(L" \t\r\n") - start + 1);
    }

    std::uint32_t RecordLimit(std::wstring value)
    {
        value = TrimSetting(std::move(value));
        if (value.empty() || value.size() > 7 || value.find_first_not_of(L"0123456789") != value.npos)
            throw hresult_invalid_argument(L"Enter a whole number from 1 to 1,048,575 for records per file.");
        auto limit = std::stoul(value);
        if (!limit || limit > 1048575)
            throw hresult_invalid_argument(L"Enter a whole number from 1 to 1,048,575 for records per file.");
        return static_cast<std::uint32_t>(limit);
    }
}

namespace winrt::HudesNative::implementation
{
    bool MainWindow::IsProtectedFolder(fs::path const& path) const
    {
        if (IsArchiveFolder(path)) return true;
        if (!m_store) return false;
        auto const& legacy = m_store->LegacyArchiveRoot();
        if (legacy.empty()) return false;
        if (::HudesNative::SamePath(path, legacy)) return true;
        return std::ranges::any_of(m_store->LegacyArchiveGroups(), [&](auto const& group) { return ::HudesNative::SamePath(path, group); });
    }

    bool MainWindow::CanManage(fs::path const& path) const
    {
        return !m_desktop.empty() && !path.empty() && !IsProtectedFolder(path) && ::HudesNative::IsWithin(path, m_desktop);
    }

    fs::path MainWindow::ArchiveLocation(::HudesNative::FileNote const& note) const
    {
        return ::HudesNative::ArchiveGroupPath(m_store->GetGroup(note.groupId) ? note.groupId : std::nullopt);
    }

    std::wstring MainWindow::ArchiveLocationName(fs::path const& path) const
    {
        if (path == ::HudesNative::ArchiveRoot()) return L"Archive";
        for (auto const& group : m_store->Groups())
            if (path == ::HudesNative::ArchiveGroupPath(group.id)) return L"Archive · " + group.name;
        return L"Archive · unGrouped";
    }

    void MainWindow::LoadWorkspaceSettings()
    {
        auto const& settings = m_store->Settings();
        NewFileBaseBox().Text(settings.newFileBase);
        std::wstring lines;
        for (auto const& preset : settings.fileExtensions)
            lines += preset.name + L" = " + preset.extension + L"\r\n";
        ExtensionPresetsBox().Text(lines);
        ExportPathBox().Text(settings.exportFolder.wstring());
        ExportFormatCombo().SelectedIndex(settings.exportFormat == L"XLSX" ? 1 : 0);
        ExportLimitBox().Text(std::to_wstring(settings.exportRecordLimit));
    }

    void MainWindow::ReadWorkspaceSettings(::HudesNative::AppSettings& settings)
    {
        auto base = TrimSetting(std::wstring(NewFileBaseBox().Text()));
        if (base.empty()) throw hresult_invalid_argument(L"Enter a beginning for new file names.");
        ::HudesNative::ValidateItemName(base + L"[1]");
        settings.newFileBase = std::move(base);
        settings.fileExtensions.clear();
        auto presetText = std::wstring(ExtensionPresetsBox().Text());
        std::ranges::replace(presetText, L'\r', L'\n');
        std::wistringstream lines(presetText);
        for (std::wstring line; std::getline(lines, line);)
        {
            line = TrimSetting(std::move(line));
            if (line.empty()) continue;
            auto equals = line.find(L'=');
            if (equals == line.npos) throw hresult_invalid_argument(L"Write each extension preset as Name = .extension.");
            auto label = TrimSetting(line.substr(0, equals));
            auto extension = TrimSetting(line.substr(equals + 1));
            if (label.empty() || extension.empty())
                throw hresult_invalid_argument(L"Each preset needs a name and an extension.");
            if (extension.front() != L'.') extension.insert(extension.begin(), L'.');
            ::HudesNative::ValidateItemName(L"file" + extension);
            settings.fileExtensions.push_back({ std::move(label), std::move(extension) });
        }
        settings.exportFolder = TrimSetting(std::wstring(ExportPathBox().Text()));
        settings.exportFormat = ExportFormatCombo().SelectedIndex() == 1 ? L"XLSX" : L"CSV";
        settings.exportRecordLimit = RecordLimit(std::wstring(ExportLimitBox().Text()));
    }

    void MainWindow::BrowseExportFolder_Click(IInspectable const&, RoutedEventArgs const&)
    {
        try
        {
            if (auto path = ::HudesNative::PickFolder(WindowHandle(), L"Choose workspace export destination", ExportPathBox().Text().c_str()))
                ExportPathBox().Text(path->wstring());
        }
        catch (...) { ShowMessage(L"Export destination", ::HudesNative::FriendlyError()); }
    }

    void MainWindow::ExportWorkspace_Click(IInspectable const&, RoutedEventArgs const&) { ExportOrCopyArchive(); }

    fire_and_forget MainWindow::ExportOrCopyArchive(fs::path selection)
    {
        auto lifetime = get_strong();
        try
        {
            if (!m_ready) co_return;
            auto archiveOnly = !selection.empty();
            fs::path destination = archiveOnly ? fs::path{} : fs::path(ExportPathBox().Text().c_str());
            if (destination.empty())
            {
                auto chosen = ::HudesNative::PickFolder(WindowHandle(),
                    archiveOnly ? L"Copy virtual folder to…" : L"Export workspace to…");
                if (!chosen) co_return;
                destination = *chosen;
                if (!archiveOnly) ExportPathBox().Text(destination.wstring());
            }
            auto xlsx = !archiveOnly && ExportFormatCombo().SelectedIndex() == 1;
            auto limit = archiveOnly ? 100000u : RecordLimit(std::wstring(ExportLimitBox().Text()));
            auto workspace = m_desktop;
            auto legacy = m_store->LegacyArchiveRoot();
            auto groups = m_store->Groups();
            auto notes = m_store->Notes();
            auto ready = std::exchange(m_ready, false);
            MainInputContainer().IsEnabled(false);
            auto title = Title();
            Title(L"Hudes 1.2.9s · Copying workspace…");
            ::HudesNative::WorkspaceExportResult result;
            std::exception_ptr error;
            auto ui = apartment_context();
            co_await resume_background();
            try { result = ::HudesNative::ExportWorkspace(workspace, legacy, destination, groups, notes, xlsx, limit, selection); }
            catch (...) { error = std::current_exception(); }
            co_await ui;
            m_ready = ready;
            MainInputContainer().IsEnabled(true);
            Title(title);
            if (error) std::rethrow_exception(error);
            auto message = result.folder.wstring();
            if (!archiveOnly)
                message += L"\n\n" + std::to_wstring(result.records) + L" records in " + std::to_wstring(result.parts) + L" metadata file(s).";
            if (result.skipped) message += L"\n" + std::to_wstring(result.skipped) + L" unavailable entries or links skipped.";
            ShowMessage(archiveOnly ? L"Archive folder copied" : L"Workspace exported", message);
        }
        catch (...) { ShowMessage(L"Export", ::HudesNative::FriendlyError()); }
    }
}

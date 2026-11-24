#pragma once

#include "MainWindow.g.h"
#include "AppStore.h"
#include "NativeServices.h"
#include "Terminal.h"
#include <atomic>
#include <map>
#include <unordered_map>

namespace winrt::HudesNative::implementation
{
    struct MainWindow : MainWindowT<MainWindow>
    {
        MainWindow();
        ~MainWindow();

        void SearchFilter_Changed(IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void ClearSearchFilters_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void Lock_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void ResumeLogin_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void HomeTab_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void SearchTab_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void SettingsTab_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void Donate_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void ThemeToggle_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void HomeFolder_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void Up_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void Refresh_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void ArchiveFolder_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void NewItem_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void Direction_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void SaveMetadata_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void CancelMetadata_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void EditNote_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void CreateGroup_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void CreateTags_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void TagPicker_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void GroupFilter_Changed(IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void DefinitionFind_Changed(IInspectable const&, Microsoft::UI::Xaml::Controls::TextChangedEventArgs const&);
        void StatusFilter_Changed(IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void Sort_Changed(IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void View_Changed(IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void FileNameFilter_Changed(IInspectable const&, Microsoft::UI::Xaml::Controls::TextChangedEventArgs const&);
        void Highlight_Changed(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void SortDirection_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void SelectMode_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void SelectGroups_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void SelectTags_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void TagFilter_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void ClearFileFilters_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void FileOptions_SizeChanged(IInspectable const&, Microsoft::UI::Xaml::SizeChangedEventArgs const&);
        void FileList_SelectionChanged(IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void FileList_DoubleTapped(IInspectable const&, Microsoft::UI::Xaml::Input::DoubleTappedRoutedEventArgs const&);
        void FileList_DragItemsStarting(IInspectable const&, Microsoft::UI::Xaml::Controls::DragItemsStartingEventArgs const&);
        void FileList_DragItemsCompleted(Microsoft::UI::Xaml::Controls::ListViewBase const&, Microsoft::UI::Xaml::Controls::DragItemsCompletedEventArgs const&);
        void FileView_DragOver(IInspectable const&, Microsoft::UI::Xaml::DragEventArgs const&);
        void FileView_Drop(IInspectable const&, Microsoft::UI::Xaml::DragEventArgs const&);
        void FileSurface_Tapped(IInspectable const&, Microsoft::UI::Xaml::Input::TappedRoutedEventArgs const&);
        void FileSurface_RightTapped(IInspectable const&, Microsoft::UI::Xaml::Input::RightTappedRoutedEventArgs const&);
        void CopyFileAddress_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void CopyLocationAddress_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void SearchBox_TextChanged(IInspectable const&, Microsoft::UI::Xaml::Controls::TextChangedEventArgs const&);
        void SearchResults_DoubleTapped(IInspectable const&, Microsoft::UI::Xaml::Input::DoubleTappedRoutedEventArgs const&);
        void BrowseDesktopFolder_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void BrowseStartFolder_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void SaveSettings_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void ResetSettings_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void BrowseExportFolder_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void ExportWorkspace_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void SettingsSavedDismiss_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void ChangePassword_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void BackupDatabase_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void RestoreDatabase_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void ProtectDatabase_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void RemoveDatabasePassword_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void Unlock_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void Hello_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void LoginPassword_KeyDown(IInspectable const&, Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const&);
        void FirstRunBrowse_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void FirstRunContinue_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void Root_KeyDown(IInspectable const&, Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const&);
        void Splitter_PointerPressed(IInspectable const&, Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
        void Splitter_PointerMoved(IInspectable const&, Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
        void Splitter_PointerReleased(IInspectable const&, Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);

    private:
        std::unique_ptr<::HudesNative::AppStore> m_store;
        ::HudesNative::DirectoryWatcher m_watcher;
        std::filesystem::path m_desktop;
        std::filesystem::path m_startFolder;
        std::filesystem::path m_currentFolder;
        std::vector<::HudesNative::FileItem> m_items;
        std::unordered_map<std::wstring, Windows::Storage::IStorageItem> m_storageItems;
        std::atomic_bool m_refreshQueued{};
        std::uint64_t m_generation{};
        std::size_t m_buildIndex{};
        bool m_ready{};
        bool m_refreshingFiles{};
        bool m_filesDirty{};
        unsigned m_fileTransfers{};
        bool m_databaseOpening{};
        bool m_helloPending{};
        std::uint64_t m_signInGeneration{};
        bool m_editing{};
        bool m_donationDialogOpen{};
        Windows::Foundation::IAsyncAction ShowDonation(bool firstStart);
        bool m_splitterDragging{};
        bool m_detailsOpen{};
        uint32_t m_resizePointer{};
        Microsoft::UI::Xaml::DispatcherTimer m_detailsTimer{nullptr};
        bool m_fontsLoaded{};
        bool m_selectMode{};
        bool m_loadingControls{};
        bool m_arrangingFileOptions{};
        std::optional<std::filesystem::path> m_lastSelectedPath;
        std::vector<std::filesystem::path> m_selectionToRestore;
        std::vector<std::wstring> m_tagFilters;
        std::vector<std::filesystem::path> m_fileDragPaths;
        double m_splitterStartX{};
        double m_splitterStartWidth{};

        void LoadSearchFilters();
        void RenderTagChips(std::vector<std::wstring> const& tags);
        void LoadShortcuts();
        winrt::fire_and_forget EditShortcut(size_t index);
        void LaunchShortcut(size_t index);
        winrt::fire_and_forget UpdateRecords(int action, std::vector<std::filesystem::path> paths,
            std::wstring description = {}, std::vector<std::wstring> tags = {},
            std::optional<std::wstring> group = {}, std::wstring status = {});
        bool CanManage(std::filesystem::path const& path) const;
        bool IsProtectedFolder(std::filesystem::path const& path) const;
        std::filesystem::path ArchiveLocation(::HudesNative::FileNote const& note) const;
        std::wstring ArchiveLocationName(std::filesystem::path const& path) const;
        winrt::fire_and_forget ExportOrCopyArchive(std::filesystem::path selection = {});
        void LoadWorkspaceSettings();
        void ReadWorkspaceSettings(::HudesNative::AppSettings& settings);
        std::wstring m_noteFilter;
        ::HudesNative::PanelNote m_noteDraft;
        bool m_notebookOpen{};
        Microsoft::UI::Xaml::DispatcherTimer m_notesTimer{ nullptr };
        WINDOWPLACEMENT m_workspacePlacement{ sizeof(WINDOWPLACEMENT) };
        bool m_compact{};
        bool m_workspaceInitialized{};
        std::optional<Windows::Foundation::Point> m_marqueeOrigin;
        bool m_marqueeActive{};
        bool m_suppressSurfaceTap{};
        bool m_shellDragRequested{};
        std::optional<Windows::Foundation::Point> m_itemDragOrigin;
        std::vector<std::filesystem::path> m_itemDragFiles;
        Microsoft::UI::Xaml::Controls::Border m_hoverFolder{ nullptr };
        void SetDropHighlight(Microsoft::UI::Xaml::Controls::Border const& folder);
        void SetupItemMove();
        winrt::fire_and_forget TransferDroppedFiles(std::vector<std::filesystem::path> paths, std::filesystem::path folder, bool internal);
        std::vector<std::filesystem::path> m_marqueeInitial;
        void SetupExplorer();
        bool PrepareFileDrag(Windows::ApplicationModel::DataTransfer::DataPackage const& data, std::vector<std::filesystem::path> paths);
        void EnableEditorClickAway(Microsoft::UI::Xaml::Controls::ContentDialog const& dialog);
        void SetupNotebook();
        bool SaveNoteDraft();
        void FinishNoteDraft();
        void OutsideNotebook();
        bool m_noteMenu{};
        int AskToSave(wchar_t const* kind);
        Windows::Foundation::IAsyncOperation<Microsoft::UI::Xaml::Controls::ContentDialogResult> ShowLightEditor(
            Microsoft::UI::Xaml::Controls::ContentDialog dialog, Microsoft::UI::Xaml::FrameworkElement anchor,
            std::function<bool()> dirty, std::function<bool()> save);
        void SetupTerminal();
        void LoadTerminalSettings();
        void UpdateTerminalPanel();
        void CloseTerminalPanel(bool explicitClose = false);
        void OpenTerminal(std::filesystem::path folder = {}, bool here = false);
        winrt::fire_and_forget InitializeTerminalView();
        void StartTerminalSession(std::wstring profile = {});
        bool m_shellMenuOpen{};
        void EndTerminalSession();
        void TerminalStyle();
        void AppendViewMenu(Microsoft::UI::Xaml::Controls::MenuFlyout const& menu, std::filesystem::path location);
        Microsoft::UI::Xaml::DispatcherTimer m_terminalTimer{ nullptr };
        Microsoft::UI::Xaml::Controls::WebView2 m_terminalWeb{nullptr};
        struct ShellTab
        {
            std::shared_ptr<::HudesNative::TerminalSession> session;
            std::filesystem::path folder;
            std::wstring profile;
        };
        std::map<unsigned, ShellTab> m_shellTabs;
        unsigned m_activeShell{}, m_nextShell{};
        bool m_switchingShell{};
        void SelectShell(unsigned id);
        void RemoveShell(unsigned id);
        void TerminalMessage(unsigned id, std::wstring const& type, std::wstring const& value = {});
        std::filesystem::path m_terminalFolder;
        std::vector<::HudesNative::ShellProfile> m_shells;
        COORD m_terminalSize{90,14};
        bool m_terminalOpen{}, m_terminalHere{}, m_terminalStarting{}, m_terminalReady{}, m_windowClosed{};

        void UpdateNotebook();
        void RenderNoteCards();
        void EditPanelNote(::HudesNative::PanelNote note);
        void RenderDraftFiles();
        void CompactWindow(bool compact);
        HWND WindowHandle() const;
        void ConfigureWindow();
        void ApplyTheme();
        void ApplyTypography();
        void ApplyTextDirection();
        void ArrangeFileOptions(double width);
        Microsoft::UI::Xaml::Media::Brush UiBrush(wchar_t const* key) const;
        Microsoft::UI::Xaml::Media::Brush FontBrush(::HudesNative::FontRole role) const;
        void LoadSettingsControls();
        struct AppearanceEditor
        {
            std::vector<Microsoft::UI::Xaml::Controls::ComboBox> families, sizes;
            std::vector<Microsoft::UI::Xaml::Controls::Button> textColors, palette;
        };
        std::array<AppearanceEditor,2> m_appearanceEditors;
        void ReadAppearance(::HudesNative::AppSettings& settings);
        void ShowSwatch(Microsoft::UI::Xaml::Controls::Button const& button, std::wstring const& color);
        void EnsureFontsLoaded();
        void LoadGroups();
        void LoadTags(std::vector<std::wstring> const& selected = {});
        void ShowTab(Microsoft::UI::Xaml::UIElement const& view, Microsoft::UI::Xaml::Controls::Button const& button);
        void UnlockSucceeded();
        void ShowSignIn();
        void NavigateTo(std::filesystem::path const& folder, bool animate = true);
        void StartWatcher();
        void QueueRefresh();
        void RefreshFiles(bool animate = false);
        void RefreshSearch(bool updateFilters = true);
        void BuildFileControls(bool animate);
        void BuildNextFileBatch(std::uint64_t generation);
        Microsoft::UI::Xaml::Controls::ListViewBase ActiveFileList();
        std::vector<std::filesystem::path> SelectedPaths();
        std::optional<std::filesystem::path> SelectedOne();
        std::optional<std::filesystem::path> PreviewPath();
        void EnsureContextSelection(std::filesystem::path const& path);
        bool IsArchiveFolder(std::filesystem::path const& path) const;
        bool IsWorkspaceLocation() const;
        std::wstring FolderLabel() const;
        void UpdateAddress();
        void ShowSelectedMetadata();
        void SetMetadataEditing(bool enabled);
        bool ResolveMetadataDraft();
        bool m_resolvingMetadata{};
        std::filesystem::path m_metadataPath;
        void BeginMetadataEdit(bool focusEditor = true);
        std::vector<std::wstring> SelectedTags();
        void UpdateTagSummary();
        void UpdateTagFilterButton();
        void SetComboText(Microsoft::UI::Xaml::Controls::ComboBox const& combo, std::wstring const& value);
        std::wstring ComboText(Microsoft::UI::Xaml::Controls::ComboBox const& combo);
        Microsoft::UI::Xaml::FrameworkElement BuildFileItem(::HudesNative::FileItem const& value, bool details);
        Microsoft::UI::Xaml::Controls::MenuFlyout BuildContextMenu(::HudesNative::FileItem const& value);
        Microsoft::UI::Xaml::Controls::MenuFlyoutSubItem NewItemSubmenu();
        void PopulateNewItemMenu(Windows::Foundation::Collections::IVector<Microsoft::UI::Xaml::Controls::MenuFlyoutItemBase> const& items);
        winrt::fire_and_forget LoadThumbnail(std::filesystem::path path, Microsoft::UI::Xaml::Controls::Image image, std::uint64_t generation);
        winrt::fire_and_forget CacheStorageItem(std::filesystem::path path, std::uint64_t generation);
        winrt::fire_and_forget HandleDrop(Microsoft::UI::Xaml::DragEventArgs args);
        std::optional<std::filesystem::path> DropFolder(Microsoft::UI::Xaml::DragEventArgs const& args);
        bool CanPinHere() const;
        void PinSelected(bool pinned);
        winrt::fire_and_forget ShowMessage(std::wstring title, std::wstring message);
        winrt::fire_and_forget CreateGroupDialog();
        winrt::fire_and_forget CreateTagsDialog();
        winrt::fire_and_forget ReminderDialog();
        winrt::fire_and_forget MetadataDialog(std::filesystem::path path);
        winrt::fire_and_forget RenameDialog();
        winrt::fire_and_forget CreateItemDialog(bool directory);
        winrt::fire_and_forget DeleteSelected();
        winrt::fire_and_forget ArchiveSelected(bool zip);
        winrt::fire_and_forget UnarchiveSelected();
        winrt::fire_and_forget CreateZipDialog();
        winrt::fire_and_forget ExtractZipDialog();
        winrt::fire_and_forget ChangePassword();
        winrt::fire_and_forget SaveSettings();
        winrt::fire_and_forget ResetSettings();
        void SelectTagFilters();
        void CloseDefinitionPanel();
        void ShowDefinitionChoices(std::wstring const& title, std::wstring const& placeholder,
            std::wstring const& action, std::vector<std::pair<std::wstring, std::wstring>> const& definitions,
            std::vector<std::wstring> const& selected,
            std::function<void(std::vector<std::wstring>)> apply, bool single = false);
        void FilterDefinitionCombo(Microsoft::UI::Xaml::Controls::ComboBox const& combo, winrt::hstring const& query);
        void ShowSettingsSavedPopup(std::wstring const& message);
        winrt::fire_and_forget PickTypographyColor(Microsoft::UI::Xaml::Controls::Button button);
        void SelectByDefinitions(bool groups);
        void SelectMatchingDefinitions(bool groups, std::vector<std::wstring> const& selectedDefinitions);
        winrt::fire_and_forget RestoreDatabase();
        winrt::fire_and_forget BackupDatabase();
        winrt::fire_and_forget InitializeDatabase();
        winrt::fire_and_forget ChangeDatabaseProtection(bool remove);
        Windows::Foundation::IAsyncOperation<IInspectable> PromptDatabasePassword(
            std::wstring title, std::wstring description, bool newPassword, bool allowUnprotected = false);
        void UpdateDatabaseProtection();
        winrt::fire_and_forget TryWindowsHello();
        void OpenSelected();
        void OpenWithSelected();
        void CopySelected(bool cut);
        winrt::fire_and_forget PasteFiles();
        void SetGroupSelected(std::optional<std::wstring> groupId);
        void SetStatusSelected(std::wstring const& status);
        void ClearReminderSelected();
        winrt::fire_and_forget CalculateSelectedHash(::HudesNative::FileHashAlgorithm algorithm);
    };
}

namespace winrt::HudesNative::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};
}

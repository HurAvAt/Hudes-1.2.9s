#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace HudesNative
{
    struct FileNote
    {
        std::filesystem::path path;
        std::wstring description;
        std::vector<std::wstring> tags;
        std::wstring updatedAt;
        std::optional<std::wstring> groupId;
        std::wstring status{ L"New" };
        bool newStatusAvailable{ true };
        std::optional<std::wstring> reminderAt;
        bool ownMetadata{};
        bool ownStatus{};
        std::vector<std::wstring> parentTags;
        bool cascadeMetadata{};
        bool cascadeGroup{};
        bool cascadeStatus{};
        std::optional<std::wstring> archiveGroupId;
        bool archived{};
        bool pinned{};
        bool legacyArchiveSource{};
    };

    struct PanelNote
    {
        std::wstring id;
        std::wstring title;
        std::wstring body;
        std::vector<std::filesystem::path> files;
        bool resolved{};
    };

    struct FileGroup
    {
        std::wstring id;
        std::wstring name;
        std::wstring color{ L"#DDE7FF" };
    };

    struct LaunchShortcut
    {
        std::wstring label;
        std::wstring type{ L"File" };
        std::wstring target;
        std::wstring icon{ L"App" };
        std::wstring iconPath;
    };

    struct FileExtensionPreset
    {
        std::wstring name;
        std::wstring extension;
    };

    struct FontSetting
    {
        std::wstring family{L"Dubai"};
        double size{11};
        std::wstring color;
    };

    struct PaletteRole { wchar_t const* key; wchar_t const* label; wchar_t const* light; wchar_t const* dark; };
    inline constexpr PaletteRole paletteRoles[]{
        {L"SurfaceBrush",L"Window background",L"#F6F8FB",L"#0D1724"},
        {L"PanelBrush",L"Panel background",L"#FFFFFF",L"#111E2C"},
        {L"CardBrush",L"Cards",L"#EFF4F9",L"#172535"},
        {L"ControlBrush",L"Buttons and controls",L"#E4ECF5",L"#1E3145"},
        {L"SelectedBrush",L"Selected controls",L"#CFE7FA",L"#2A4B65"},
        {L"AccentBrush",L"Icons",L"#396C91",L"#8EC5EB"},
        {L"FocusLineBrush",L"Focus and selection lines",L"#396C91",L"#8EC5EB"},
        {L"BorderBrush",L"Borders and dividers",L"#CDD8E5",L"#344A60"},
        {L"MutedBrush",L"Secondary labels",L"#536579",L"#CAD7E5"},
        {L"DangerBrush",L"Errors and reminders",L"#B3261E",L"#FFB4AB"},
        {L"ResolvedBrush",L"Resolved checkmark",L"#187544",L"#50DB8A"},
        {L"ArchiveBrush",L"Archive icon and highlight",L"#3488D1",L"#5EA9E7"},
        {L"FieldBrush",L"Input background",L"#F9FBFD",L"#1C2B3B"},
        {L"HoverBrush",L"Input hover",L"#FFFFFF",L"#223548"},
        {L"FocusBrush",L"Focused input",L"#FFFFFF",L"#24394E"},
        {L"FieldBorderBrush",L"Input border",L"#AFC1D2",L"#49647E"},
        {L"DoneBrush",L"Done status",L"#1B8A4B",L"#228750"},
        {L"OngoingBrush",L"Ongoing status",L"#2574D9",L"#367DD8"},
        {L"ImportantBrush",L"Important status",L"#C62828",L"#BD3737"},
        {L"NewStatusBrush",L"New status",L"#A57200",L"#A47200"},
        {L"UrgentBrush",L"Emergency status",L"#B52A20",L"#BA3C32"},
        {L"BadgeInkBrush",L"Status symbols",L"#FFFFFF",L"#FFFFFF"},
        {L"TerminalBrush",L"Terminal background",L"#121923",L"#121923"},
        {L"UngroupedBrush",L"Ungrouped archive",L"#9DAFC4",L"#9DAFC4"},
    };
    inline constexpr wchar_t const* fontRoles[]{L"App controls", L"File view", L"Descriptions and groups", L"Search", L"File note preview", L"Tags", L"Search group names", L"Terminal"};
    enum FontRole : size_t { AppFont, FileFont, DescriptionFont, SearchFont, PreviewFont, TagFont, GroupFont, ShellFont };
    struct Appearance
    {
        std::array<FontSetting,std::size(fontRoles)> fonts;
        std::array<std::wstring,std::size(paletteRoles)> colors;
        explicit Appearance(bool dark=false)
        {
            for (auto& font : fonts) font.color=dark ? L"#FFFFFF" : L"#0B1727";
            fonts[TagFont].color=dark ? L"#FF7A7A" : L"#9B1C1C";
            fonts[GroupFont].size=10;
            fonts[ShellFont]={L"Consolas",14,L"#E5EBF2"};
            for (size_t i=0;i<colors.size();++i) colors[i]=dark ? paletteRoles[i].dark : paletteRoles[i].light;
        }
        std::wstring const& Color(std::wstring_view key) const
        {
            for (size_t i=0;i<colors.size();++i) if (key==paletteRoles[i].key) return colors[i];
            return fonts[AppFont].color;
        }
    };

    struct AppSettings
    {
        std::array<Appearance,2> appearance{Appearance(false),Appearance(true)};
        Appearance const& Look() const { return appearance[themeMode==L"Dark" ? 1 : 0]; }
        bool detailsPinned{true};
        double detailsWidth{268}, notesWidth{302}, terminalHeight{238};
        bool notesEnabled{ true };
        bool notesPinned{};
        bool showHidden{};
        bool terminalEnabled{ true };
        bool terminalPinned{};
        std::wstring terminalShell{ L"cmd" };
        bool highlightGroupColors{ true };
        double reminderWarningHours{ 24.0 };
        std::filesystem::path desktopFolder;
        bool desktopFolderConfirmed{ false };
        std::filesystem::path defaultFolder;
        bool metadataRtl{ false };
        std::wstring sortMethod{ L"Name" };
        bool sortDescending{ false };
        std::wstring viewMode{ L"Large icons" };
        std::wstring themeMode{ L"Light" };
        std::wstring passwordSalt;
        std::wstring passwordHash;
        bool useWindowsHello{ false };
        bool passwordRequired{ true };
        std::array<LaunchShortcut, 12> shortcuts{};
        std::wstring bashExecutable;
        std::wstring newFileBase{ L"newFile" };
        std::vector<FileExtensionPreset> fileExtensions{
            { L"Excel", L".xlsx" }, { L"PowerPoint", L".pptx" }, { L"Word", L".docx" },
            { L"Access", L".accdb" }, { L"Text", L".txt" } };
        std::filesystem::path exportFolder;
        std::wstring exportFormat{ L"CSV" };
        std::uint32_t exportRecordLimit{ 100000 };
    };

    struct Database
    {
        int version{ 6 };
        std::vector<FileNote> notes;
        std::vector<FileGroup> groups;
        std::vector<PanelNote> panelNotes;
        std::vector<std::wstring> tagDefinitions;
        AppSettings settings;
        std::filesystem::path legacyArchiveRoot;
        std::vector<std::filesystem::path> legacyArchiveGroups;
    };

    struct FileItem
    {
        bool pinned{};
        std::filesystem::path path;
        std::wstring name;
        std::wstring type;
        std::uintmax_t size{};
        std::filesystem::file_time_type modified{};
        bool isFolder{};
        bool isArchive{};
        bool managed{};
        std::wstring archiveColor;
        std::wstring status{ L"New" };
        std::wstring groupName{ L"Ungrouped" };
        std::wstring groupColor;
        bool reminderNear{};
    };

    enum class MetadataKind
    {
        Text,
        TextList,
        UnsignedInteger
    };

    struct MetadataEntry
    {
        std::wstring category;
        std::wstring name;
        std::wstring canonicalName;
        std::wstring value;
        std::wstring originalValue;
        bool editable{};
        MetadataKind kind{ MetadataKind::Text };
    };
}

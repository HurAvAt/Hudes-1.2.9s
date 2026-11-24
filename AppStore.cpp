#include "pch.h"
#include "AppStore.h"

#include <bcrypt.h>
#include <wincrypt.h>
#include <winrt/Windows.Data.Json.h>
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")

using namespace winrt;
using namespace Windows::Data::Json;
using HudesNative::AppSettings;
using HudesNative::Database;
using HudesNative::FileGroup;
using HudesNative::FileNote;
using HudesNative::PanelNote;

namespace
{
    std::wstring Trim(std::wstring value)
    {
        auto const first = value.find_first_not_of(L" \t\r\n");
        if (first == std::wstring::npos) return {};
        auto const last = value.find_last_not_of(L" \t\r\n");
        return value.substr(first, last - first + 1);
    }

    std::wstring Lower(std::wstring value)
    {
        std::ranges::transform(value, value.begin(), [](wchar_t value)
        {
            return static_cast<wchar_t>(::towlower(value));
        });
        return value;
    }

    bool EqualText(std::wstring const& left, std::wstring const& right)
    {
        return _wcsicmp(left.c_str(), right.c_str()) == 0;
    }

    constexpr wchar_t const* fontKeys[]{L"App",L"File",L"Description",L"Search",L"NotePreview",L"Tag",L"SearchGroup",L"Terminal"};
    constexpr auto folderTag = L"Folder";
    constexpr auto subfolderTag = L"SubFolder";
    constexpr auto subfileTag = L"SubFile";
    constexpr std::array automaticTags{ folderTag, subfolderTag, subfileTag };

    bool IsReservedTag(std::wstring const& tag)
    {
        return HudesNative::IsAutomaticTag(tag) || EqualText(tag, L"SUB");
    }

    bool ContainsTag(std::vector<std::wstring> const& tags, std::wstring const& value)
    {
        return std::ranges::any_of(tags, [&](auto const& tag) { return EqualText(tag,value); });
    }

    void SetAutomaticTag(FileNote& note, wchar_t const* tag = nullptr)
    {
        std::erase_if(note.tags, IsReservedTag);
        if (tag) note.tags.emplace_back(tag);
    }

    void UpgradeAutomaticTags(Database& database)
    {
        std::unordered_set<std::wstring> occupiedNames;
        std::unordered_set<std::wstring> recordedDirectories;
        for (auto const& tag : database.tagDefinitions) occupiedNames.insert(Lower(tag));
        for (auto const& note : database.notes)
        {
            for (auto const& tag : note.tags) occupiedNames.insert(Lower(tag));
            auto path = note.path.lexically_normal();
            for (auto parent = path.parent_path(); parent != path && !parent.empty(); parent = path.parent_path())
            {
                if (!recordedDirectories.insert(Lower(parent.wstring())).second) break;
                path = parent;
            }
        }

        // Old user tags with the new reserved names keep their assignments under a custom name.
        std::unordered_map<std::wstring, std::wstring> replacements;
        for (auto tag : automaticTags)
        {
            auto key = Lower(tag);
            if (!occupiedNames.contains(key)) continue;
            auto label = std::wstring(tag) + L" (custom)";
            auto candidate = label;
            for (int number = 2; !occupiedNames.insert(Lower(candidate)).second; ++number)
                candidate = label + L" " + std::to_wstring(number);
            replacements.emplace(std::move(key), std::move(candidate));
        }
        auto keepCustomTag = [&](std::wstring& tag)
        {
            if (auto replacement = replacements.find(Lower(tag)); replacement != replacements.end())
                tag = replacement->second;
        };
        for (auto& tag : database.tagDefinitions) keepCustomTag(tag);
        for (auto& note : database.notes)
        {
            auto inherited = std::ranges::any_of(note.tags, [](auto const& tag) { return EqualText(tag, L"SUB"); });
            for (auto& tag : note.tags) keepCustomTag(tag);
            if (!note.cascadeMetadata && !inherited) continue;

            auto attributes = GetFileAttributesW(note.path.c_str());
            auto directory = attributes != INVALID_FILE_ATTRIBUTES
                ? (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0
                : note.cascadeMetadata || note.cascadeGroup || note.cascadeStatus || note.archiveGroupId.has_value() ||
                    recordedDirectories.contains(Lower(note.path.lexically_normal().wstring()));
            if (!directory) note.cascadeMetadata = note.cascadeGroup = note.cascadeStatus = false;
            auto tag = note.cascadeMetadata ? folderTag : inherited ? (directory ? subfolderTag : subfileTag) : nullptr;
            SetAutomaticTag(note, tag);
        }
    }

    std::filesystem::path Normalize(std::filesystem::path const& value)
    {
        try { return std::filesystem::weakly_canonical(std::filesystem::absolute(value)); }
        catch (...) { return std::filesystem::absolute(value).lexically_normal(); }
    }

    bool SamePath(std::filesystem::path const& left, std::filesystem::path const& right)
    {
        return EqualText(Normalize(left).wstring(), Normalize(right).wstring());
    }

    // Windows paths compare without case; retain the original descendant spelling.
    std::optional<std::filesystem::path> RelativeInside(std::filesystem::path const& value, std::filesystem::path const& root)
    {
        auto path = Normalize(value), parent = Normalize(root);
        auto part = path.begin();
        for (auto expected = parent.begin(); expected != parent.end(); ++expected, ++part)
            if (part == path.end() || !EqualText(part->wstring(), expected->wstring())) return std::nullopt;
        std::filesystem::path relative;
        for (; part != path.end(); ++part) relative /= *part;
        return relative;
    }

    std::wstring NowText()
    {
        SYSTEMTIME time{};
        GetLocalTime(&time);
        wchar_t buffer[40]{};
        swprintf_s(buffer, L"%04hu-%02hu-%02huT%02hu:%02hu:%02hu", time.wYear, time.wMonth,
            time.wDay, time.wHour, time.wMinute, time.wSecond);
        return buffer;
    }

    hstring NamedString(JsonObject const& object, wchar_t const* name, std::wstring const& fallback = {})
    {
        try { return object.GetNamedString(name, hstring{ fallback }); }
        catch (...) { return hstring{ fallback }; }
    }

    bool NamedBool(JsonObject const& object, wchar_t const* name, bool fallback)
    {
        try { return object.GetNamedBoolean(name, fallback); }
        catch (...) { return fallback; }
    }

    double NamedNumber(JsonObject const& object, wchar_t const* name, double fallback)
    {
        try { return object.GetNamedNumber(name, fallback); }
        catch (...) { return fallback; }
    }

    std::optional<std::wstring> NamedOptionalString(JsonObject const& object, wchar_t const* name)
    {
        try
        {
            auto value = object.GetNamedValue(name);
            if (value.ValueType() == JsonValueType::Null) return std::nullopt;
            auto text = value.GetString();
            if (text.empty()) return std::nullopt;
            return std::wstring{ text };
        }
        catch (...) { return std::nullopt; }
    }

    JsonValue StringValue(std::wstring const& value)
    {
        return JsonValue::CreateStringValue(value);
    }

    JsonValue OptionalValue(std::optional<std::wstring> const& value)
    {
        return value ? JsonValue::CreateStringValue(*value) : JsonValue::CreateNullValue();
    }

    Database ReadDatabase(std::string_view fileData)
    {
        if (fileData.starts_with("\xEF\xBB\xBF")) fileData.remove_prefix(3);
        auto root = JsonObject::Parse(to_hstring(fileData));
        if (!root.HasKey(L"Notes") || root.GetNamedValue(L"Notes").ValueType() != JsonValueType::Array ||
            !root.HasKey(L"Settings") || root.GetNamedValue(L"Settings").ValueType() != JsonValueType::Object)
            throw hresult_error(E_INVALIDARG, L"This file is not a Hudes database.");
        Database database;
        database.version = static_cast<int>(NamedNumber(root, L"Version", 2));
        if (root.HasKey(L"PanelNotes"))
            for (auto const& raw : root.GetNamedArray(L"PanelNotes"))
            {
                auto object = raw.GetObject();
                PanelNote note;
                note.id = NamedString(object, L"Id");
                note.title = NamedString(object, L"Title");
                note.body = NamedString(object, L"Body");
                note.resolved = NamedBool(object, L"Resolved", false);
                for (auto const& file : object.GetNamedArray(L"Files")) note.files.emplace_back(file.GetString().c_str());
                if (!note.id.empty()) database.panelNotes.push_back(std::move(note));
            }
        database.legacyArchiveRoot = std::wstring(NamedString(root, L"LegacyArchiveRoot"));
        if (root.HasKey(L"LegacyArchiveGroups"))
            for (auto const& value : root.GetNamedArray(L"LegacyArchiveGroups")) database.legacyArchiveGroups.emplace_back(std::wstring(value.GetString()));

        try
        {
            for (auto const& value : root.GetNamedArray(L"Notes"))
            {
                auto object = value.GetObject();
                FileNote note;
                note.path = std::wstring{ NamedString(object, L"Path") };
                note.description = NamedString(object, L"Description");
                note.updatedAt = NamedString(object, L"UpdatedAt", NowText());
                note.groupId = NamedOptionalString(object, L"GroupId");
                note.status = NamedString(object, L"Status", L"New");
                note.newStatusAvailable = NamedBool(object, L"NewStatusAvailable", true);
                note.reminderAt = NamedOptionalString(object, L"ReminderAt");
                note.ownMetadata = NamedBool(object, L"OwnMetadata", false);
                note.ownStatus = NamedBool(object, L"OwnStatus", false);
                if (object.HasKey(L"ParentTags"))
                    for (auto const& tag : object.GetNamedArray(L"ParentTags")) note.parentTags.emplace_back(tag.GetString());
                note.cascadeMetadata = NamedBool(object, L"CascadeMetadata", false);
                note.cascadeGroup = NamedBool(object, L"CascadeGroup", false);
                note.cascadeStatus = NamedBool(object, L"CascadeStatus", false);
                note.archiveGroupId = NamedOptionalString(object, L"ArchiveGroupId");
                note.archived = NamedBool(object, L"Archived", false);
                note.pinned = NamedBool(object, L"Pinned", false);
                note.legacyArchiveSource = NamedBool(object, L"LegacyArchiveSource", false);
                try
                {
                    for (auto const& tag : object.GetNamedArray(L"Tags"))
                        note.tags.emplace_back(tag.GetString());
                }
                catch (...) {}
                if (!note.path.empty()) database.notes.push_back(std::move(note));
            }
        }
        catch (...) {}

        try
        {
            for (auto const& value : root.GetNamedArray(L"Groups"))
            {
                auto object = value.GetObject();
                FileGroup group;
                group.id = NamedString(object, L"Id");
                group.name = NamedString(object, L"Name");
                group.color = NamedString(object, L"Color", L"#DDE7FF");
                if (!group.id.empty() && !group.name.empty()) database.groups.push_back(std::move(group));
            }
        }
        catch (...) {}

        try
        {
            for (auto const& value : root.GetNamedArray(L"TagDefinitions"))
                database.tagDefinitions.emplace_back(value.GetString());
        }
        catch (...) {}

        try
        {
            auto settings = root.GetNamedObject(L"Settings");
            auto& target = database.settings;
            target.detailsPinned = NamedBool(settings,L"DetailsPinned",true);
            target.detailsWidth = std::clamp(NamedNumber(settings,L"DetailsWidth",268),180.0,1600.0);
            target.notesWidth = std::clamp(NamedNumber(settings,L"NotesWidth",302),180.0,1600.0);
            target.terminalHeight = std::clamp(NamedNumber(settings,L"TerminalHeight",238),125.0,1600.0);
            target.notesEnabled = NamedBool(settings, L"NotesEnabled", true);
            target.notesPinned = NamedBool(settings, L"NotesPinned", false);
            target.showHidden = NamedBool(settings, L"ShowHidden", false);
            target.terminalEnabled = NamedBool(settings, L"TerminalEnabled", true);
            target.terminalPinned = NamedBool(settings, L"TerminalPinned", false);
            target.terminalShell = NamedString(settings, L"TerminalShell", L"cmd");
            target.highlightGroupColors = NamedBool(settings, L"HighlightGroupColors", true);
            target.reminderWarningHours = NamedNumber(settings, L"ReminderWarningHours", 24.0);
            target.desktopFolder = std::wstring{ NamedString(settings, L"DesktopFolder") };
            target.desktopFolderConfirmed = NamedBool(settings, L"DesktopFolderConfirmed", false);
            target.defaultFolder = std::wstring{ NamedString(settings, L"DefaultFolder") };
            target.metadataRtl = NamedBool(settings, L"MetadataRtl", false);
            for (size_t mode=0;mode<2;++mode)
            {
                auto& look=target.appearance[mode];
                auto key=mode ? L"DarkAppearance" : L"LightAppearance";
                auto profile=settings.GetNamedObject(key, JsonObject());
                auto fontData=profile.GetNamedObject(L"Fonts",JsonObject());
                for (size_t i=0;i<look.fonts.size();++i)
                {
                    auto& font=look.fonts[i];
                    std::wstring prefix=fontKeys[i];
                    auto terminal=i==HudesNative::ShellFont;
                    auto legacyFamily=NamedString(settings,(terminal ? L"TerminalFont" : prefix+L"FontFamily").c_str(),font.family);
                    auto legacySize=NamedNumber(settings,(terminal ? L"TerminalSize" : prefix+L"FontSize").c_str(),font.size);
                    auto legacyColor=NamedString(settings,(terminal ? L"TerminalForeground" : prefix+(mode ? L"TextColorDark" : L"TextColorLight")).c_str(),font.color);
                    auto entry=fontData.GetNamedObject(prefix,JsonObject());
                    font.family=NamedString(entry,L"Family",std::wstring(legacyFamily));
                    font.size=std::clamp(NamedNumber(entry,L"Size",legacySize),1.0,200.0);
                    font.color=NamedString(entry,L"Color",std::wstring(legacyColor));
                }
                for (size_t i=0;i<look.colors.size();++i)
                {
                    auto role=HudesNative::paletteRoles[i].key;
                    if (std::wstring_view(role)==L"TerminalBrush") look.colors[i]=NamedString(settings,L"TerminalBackground",look.colors[i]);
                    look.colors[i]=NamedString(profile,role,look.colors[i]);
                }
            }
            target.sortMethod = NamedString(settings, L"SortMethod", L"Name");
            target.sortDescending = NamedBool(settings, L"SortDescending", false);
            target.viewMode = NamedString(settings, L"ViewMode", L"Large icons");
            target.themeMode = NamedString(settings, L"ThemeMode", L"Light");
            target.passwordSalt = NamedString(settings, L"PasswordSalt");
            target.passwordHash = NamedString(settings, L"PasswordHash");
            target.useWindowsHello = NamedBool(settings, L"UseWindowsHello", false);
            target.passwordRequired = NamedBool(settings, L"PasswordRequired", true);
            target.bashExecutable = NamedString(settings, L"BashExecutable");
            target.newFileBase = NamedString(settings, L"NewFileBase", L"newFile");
            target.exportFolder = std::wstring(NamedString(settings, L"ExportFolder"));
            target.exportFormat = NamedString(settings, L"ExportFormat", L"CSV");
            auto recordLimit = NamedNumber(settings, L"ExportRecordLimit", 100000);
            if (recordLimit >= 1 && recordLimit <= 1048575) target.exportRecordLimit = static_cast<std::uint32_t>(recordLimit);
            if (settings.HasKey(L"FileExtensions"))
            {
                auto saved = settings.GetNamedArray(L"FileExtensions");
                target.fileExtensions.clear();
                for (auto const& value : saved)
                {
                    auto item = value.GetObject();
                    target.fileExtensions.push_back({ std::wstring(NamedString(item, L"Name")), std::wstring(NamedString(item, L"Extension")) });
                }
            }
            if (settings.HasKey(L"Shortcuts"))
            {
                auto shortcuts = settings.GetNamedArray(L"Shortcuts");
                auto count = (std::min)(shortcuts.Size(), static_cast<uint32_t>(target.shortcuts.size()));
                for (uint32_t index = 0; index < count; ++index)
                {
                    auto object = shortcuts.GetObjectAt(index);
                    auto& shortcut = target.shortcuts[index];
                    shortcut.label = NamedString(object, L"Label");
                    shortcut.type = NamedString(object, L"Type", L"File");
                    shortcut.target = NamedString(object, L"Target");
                    shortcut.icon = NamedString(object, L"Icon", L"App");
                    shortcut.iconPath = NamedString(object, L"IconPath");
                }
            }
        }
        catch (...) {}
        return database;
    }

    std::string SerializeDatabase(Database const& database)
    {
        JsonObject root;
        root.Insert(L"Version", JsonValue::CreateNumberValue(database.version));
        root.Insert(L"LegacyArchiveRoot", StringValue(database.legacyArchiveRoot.wstring()));
        JsonArray notebook;
        for (auto const& note : database.panelNotes)
        {
            JsonObject item;
            item.Insert(L"Id", StringValue(note.id));
            item.Insert(L"Title", StringValue(note.title));
            item.Insert(L"Body", StringValue(note.body));
            item.Insert(L"Resolved", JsonValue::CreateBooleanValue(note.resolved));
            JsonArray files;
            for (auto const& file : note.files) files.Append(StringValue(file.wstring()));
            item.Insert(L"Files", files);
            notebook.Append(item);
        }
        root.Insert(L"PanelNotes", notebook);
        JsonArray archivedGroups;
        for (auto const& path : database.legacyArchiveGroups) archivedGroups.Append(StringValue(path.wstring()));
        root.Insert(L"LegacyArchiveGroups", archivedGroups);

        JsonArray notes;
        for (auto const& note : database.notes)
        {
            JsonObject object;
            object.Insert(L"Path", StringValue(note.path.wstring()));
            object.Insert(L"Description", StringValue(note.description));
            JsonArray tags;
            for (auto const& tag : note.tags) tags.Append(StringValue(tag));
            object.Insert(L"Tags", tags);
            object.Insert(L"UpdatedAt", StringValue(note.updatedAt));
            object.Insert(L"GroupId", OptionalValue(note.groupId));
            object.Insert(L"Status", StringValue(note.status));
            object.Insert(L"NewStatusAvailable", JsonValue::CreateBooleanValue(note.newStatusAvailable));
            object.Insert(L"ReminderAt", OptionalValue(note.reminderAt));
            object.Insert(L"OwnMetadata", JsonValue::CreateBooleanValue(note.ownMetadata));
            object.Insert(L"OwnStatus", JsonValue::CreateBooleanValue(note.ownStatus));
            JsonArray offered;
            for (auto const& tag : note.parentTags) offered.Append(StringValue(tag));
            object.Insert(L"ParentTags", offered);
            object.Insert(L"CascadeMetadata", JsonValue::CreateBooleanValue(note.cascadeMetadata));
            object.Insert(L"CascadeGroup", JsonValue::CreateBooleanValue(note.cascadeGroup));
            object.Insert(L"CascadeStatus", JsonValue::CreateBooleanValue(note.cascadeStatus));
            object.Insert(L"ArchiveGroupId", OptionalValue(note.archiveGroupId));
            object.Insert(L"Archived", JsonValue::CreateBooleanValue(note.archived));
            if (note.pinned) object.Insert(L"Pinned", JsonValue::CreateBooleanValue(true));
            object.Insert(L"LegacyArchiveSource", JsonValue::CreateBooleanValue(note.legacyArchiveSource));
            notes.Append(object);
        }
        root.Insert(L"Notes", notes);

        JsonArray groups;
        for (auto const& group : database.groups)
        {
            JsonObject object;
            object.Insert(L"Id", StringValue(group.id));
            object.Insert(L"Name", StringValue(group.name));
            object.Insert(L"Color", StringValue(group.color));
            groups.Append(object);
        }
        root.Insert(L"Groups", groups);

        JsonArray tags;
        for (auto const& tag : database.tagDefinitions) tags.Append(StringValue(tag));
        root.Insert(L"TagDefinitions", tags);

        auto const& value = database.settings;
        JsonObject settings;
        settings.Insert(L"DetailsPinned", JsonValue::CreateBooleanValue(value.detailsPinned));
        settings.Insert(L"DetailsWidth", JsonValue::CreateNumberValue(value.detailsWidth));
        settings.Insert(L"NotesWidth", JsonValue::CreateNumberValue(value.notesWidth));
        settings.Insert(L"TerminalHeight", JsonValue::CreateNumberValue(value.terminalHeight));
        for (size_t mode=0;mode<2;++mode)
        {
            JsonObject profile, fonts;
            auto const& look=value.appearance[mode];
            for (size_t i=0;i<look.fonts.size();++i)
            {
                JsonObject entry;
                entry.Insert(L"Family",StringValue(look.fonts[i].family));
                entry.Insert(L"Size",JsonValue::CreateNumberValue(look.fonts[i].size));
                entry.Insert(L"Color",StringValue(look.fonts[i].color));
                fonts.Insert(fontKeys[i],entry);
            }
            profile.Insert(L"Fonts",fonts);
            for (size_t i=0;i<look.colors.size();++i) profile.Insert(HudesNative::paletteRoles[i].key,StringValue(look.colors[i]));
            settings.Insert(mode ? L"DarkAppearance" : L"LightAppearance",profile);
        }
        settings.Insert(L"NotesEnabled", JsonValue::CreateBooleanValue(value.notesEnabled));
        settings.Insert(L"NotesPinned", JsonValue::CreateBooleanValue(value.notesPinned));
        settings.Insert(L"ShowHidden", JsonValue::CreateBooleanValue(value.showHidden));
        settings.Insert(L"TerminalEnabled", JsonValue::CreateBooleanValue(value.terminalEnabled));
        settings.Insert(L"TerminalPinned", JsonValue::CreateBooleanValue(value.terminalPinned));
        settings.Insert(L"TerminalShell", StringValue(value.terminalShell));
        settings.Insert(L"HighlightGroupColors", JsonValue::CreateBooleanValue(value.highlightGroupColors));
        settings.Insert(L"ReminderWarningHours", JsonValue::CreateNumberValue(value.reminderWarningHours));
        settings.Insert(L"DesktopFolder", StringValue(value.desktopFolder.wstring()));
        settings.Insert(L"DesktopFolderConfirmed", JsonValue::CreateBooleanValue(value.desktopFolderConfirmed));
        settings.Insert(L"DefaultFolder", StringValue(value.defaultFolder.wstring()));
        settings.Insert(L"MetadataRtl", JsonValue::CreateBooleanValue(value.metadataRtl));
        settings.Insert(L"SortMethod", StringValue(value.sortMethod));
        settings.Insert(L"SortDescending", JsonValue::CreateBooleanValue(value.sortDescending));
        settings.Insert(L"ViewMode", StringValue(value.viewMode));
        settings.Insert(L"ThemeMode", StringValue(value.themeMode));
        settings.Insert(L"PasswordSalt", StringValue(value.passwordSalt));
        settings.Insert(L"PasswordHash", StringValue(value.passwordHash));
        settings.Insert(L"UseWindowsHello", JsonValue::CreateBooleanValue(value.useWindowsHello));
        settings.Insert(L"PasswordRequired", JsonValue::CreateBooleanValue(value.passwordRequired));
        settings.Insert(L"BashExecutable", StringValue(value.bashExecutable));
        settings.Insert(L"NewFileBase", StringValue(value.newFileBase));
        settings.Insert(L"ExportFolder", StringValue(value.exportFolder.wstring()));
        settings.Insert(L"ExportFormat", StringValue(value.exportFormat));
        settings.Insert(L"ExportRecordLimit", JsonValue::CreateNumberValue(value.exportRecordLimit));
        JsonArray extensions;
        for (auto const& preset : value.fileExtensions)
        {
            JsonObject item;
            item.Insert(L"Name", StringValue(preset.name));
            item.Insert(L"Extension", StringValue(preset.extension));
            extensions.Append(item);
        }
        settings.Insert(L"FileExtensions", extensions);
        JsonArray shortcuts;
        for (auto const& shortcut : value.shortcuts)
        {
            JsonObject object;
            object.Insert(L"Label", StringValue(shortcut.label));
            object.Insert(L"Type", StringValue(shortcut.type));
            object.Insert(L"Target", StringValue(shortcut.target));
            object.Insert(L"Icon", StringValue(shortcut.icon));
            object.Insert(L"IconPath", StringValue(shortcut.iconPath));
            shortcuts.Append(object);
        }
        settings.Insert(L"Shortcuts", shortcuts);
        root.Insert(L"Settings", settings);
        return to_string(root.Stringify());
    }

    std::vector<std::uint8_t> Utf8(std::wstring const& value)
    {
        if (value.empty()) return {};
        auto const count = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
        std::vector<std::uint8_t> result(static_cast<size_t>(count));
        WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), reinterpret_cast<char*>(result.data()), count, nullptr, nullptr);
        return result;
    }

    std::wstring Base64(std::span<std::uint8_t const> bytes)
    {
        DWORD count{};
        CryptBinaryToStringW(bytes.data(), static_cast<DWORD>(bytes.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &count);
        std::wstring result(count, L'\0');
        check_bool(CryptBinaryToStringW(bytes.data(), static_cast<DWORD>(bytes.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, result.data(), &count));
        if (!result.empty() && result.back() == L'\0') result.pop_back();
        return result;
    }

    std::vector<std::uint8_t> FromBase64(std::wstring const& value)
    {
        DWORD count{};
        check_bool(CryptStringToBinaryW(value.c_str(), static_cast<DWORD>(value.size()), CRYPT_STRING_BASE64, nullptr, &count, nullptr, nullptr));
        std::vector<std::uint8_t> result(count);
        check_bool(CryptStringToBinaryW(value.c_str(), static_cast<DWORD>(value.size()), CRYPT_STRING_BASE64, result.data(), &count, nullptr, nullptr));
        result.resize(count);
        return result;
    }

    std::vector<std::uint8_t> PasswordHash(std::wstring const& password, std::span<std::uint8_t const> salt)
    {
        BCRYPT_ALG_HANDLE raw{};
        check_nt(BCryptOpenAlgorithmProvider(&raw, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG));
        struct ProviderCloser { void operator()(void* value) const noexcept { if (value) BCryptCloseAlgorithmProvider(value, 0); } };
        std::unique_ptr<void, ProviderCloser> provider(raw);
        auto passwordBytes = Utf8(password);
        std::vector<std::uint8_t> result(32);
        check_nt(BCryptDeriveKeyPBKDF2(raw, passwordBytes.data(), static_cast<ULONG>(passwordBytes.size()),
            const_cast<PUCHAR>(salt.data()), static_cast<ULONG>(salt.size()), 160000,
            result.data(), static_cast<ULONG>(result.size()), 0));
        return result;
    }

    bool FixedEqual(std::span<std::uint8_t const> left, std::span<std::uint8_t const> right)
    {
        if (left.size() != right.size()) return false;
        std::uint8_t difference{};
        for (size_t index = 0; index < left.size(); ++index) difference |= left[index] ^ right[index];
        return difference == 0;
    }

    std::wstring NewId()
    {
        GUID value{};
        check_hresult(CoCreateGuid(&value));
        wchar_t buffer[33]{};
        swprintf_s(buffer, L"%08x%04x%04x%02x%02x%02x%02x%02x%02x%02x%02x", value.Data1, value.Data2,
            value.Data3, value.Data4[0], value.Data4[1], value.Data4[2], value.Data4[3], value.Data4[4], value.Data4[5], value.Data4[6], value.Data4[7]);
        return buffer;
    }

    std::vector<std::wstring> Terms(std::wstring const& query)
    {
        std::wistringstream input(query);
        std::vector<std::wstring> values;
        for (std::wstring value; input >> value;) values.push_back(Lower(value));
        return values;
    }
}

namespace HudesNative
{
    bool IsAutomaticTag(std::wstring const& tag) noexcept
    {
        return std::ranges::any_of(automaticTags, [&](auto name) { return _wcsicmp(tag.c_str(), name) == 0; });
    }

    AppStore::AppStore(std::filesystem::path databasePath, std::wstring_view databasePassword,
        std::optional<std::wstring_view> migrationPassword)
    {
        if (databasePath.empty())
        {
            wchar_t module[MAX_PATH]{};
            GetModuleFileNameW(nullptr, module, MAX_PATH);
            auto folder = std::filesystem::path(module).parent_path();
            databasePath = folder / L"hudes.db.json";
            auto legacy = folder / L"salif.db.json";
            if (!std::filesystem::exists(databasePath) && std::filesystem::is_regular_file(legacy))
                std::filesystem::copy_file(legacy, databasePath, std::filesystem::copy_options::skip_existing);
        }
        m_databasePath = std::move(databasePath);
        LoadOrCreate(databasePassword, migrationPassword);
    }

    std::filesystem::path const& AppStore::DatabasePath() const noexcept { return m_databasePath; }
    AppSettings& AppStore::Settings() noexcept { return m_database.settings; }
    AppSettings const& AppStore::Settings() const noexcept { return m_database.settings; }
    std::vector<FileGroup> const& AppStore::Groups() const noexcept { return m_database.groups; }
    std::vector<std::wstring> const& AppStore::Tags() const noexcept { return m_database.tagDefinitions; }
    std::vector<FileNote> const& AppStore::Notes() const noexcept { return m_database.notes; }
    std::filesystem::path const& AppStore::LegacyArchiveRoot() const noexcept { return m_database.legacyArchiveRoot; }
    std::vector<std::filesystem::path> const& AppStore::LegacyArchiveGroups() const noexcept { return m_database.legacyArchiveGroups; }

    bool AppStore::IsArchived(std::filesystem::path const& path) const noexcept
    {
        auto note = Get(path);
        return note && note->archived;
    }

    void AppStore::SetArchived(std::span<std::filesystem::path const> paths, bool archived)
    {
        std::scoped_lock guard(m_mutex);
        for (auto const& path : paths)
        {
            auto& note = GetOrCreateLocked(path);
            note.archived = archived;
            note.status = archived ? L"Done" : L"Ongoing";
            note.newStatusAvailable = false;
            note.updatedAt = NowText();
        }
        PersistLocked();
    }

    FileNote* AppStore::Get(std::filesystem::path const& path) noexcept
    {
        auto found = std::ranges::find_if(m_database.notes, [&](auto const& note) { return SamePath(note.path, path); });
        return found == m_database.notes.end() ? nullptr : &*found;
    }

    FileNote const* AppStore::Get(std::filesystem::path const& path) const noexcept
    {
        auto found = std::ranges::find_if(m_database.notes, [&](auto const& note) { return SamePath(note.path, path); });
        return found == m_database.notes.end() ? nullptr : &*found;
    }

    FileGroup const* AppStore::GetGroup(std::optional<std::wstring> const& id) const noexcept
    {
        if (!id) return nullptr;
        auto found = std::ranges::find_if(m_database.groups, [&](auto const& group) { return group.id == *id; });
        return found == m_database.groups.end() ? nullptr : &*found;
    }

    std::vector<FileNote const*> AppStore::Search(std::wstring const& query) const
    {
        auto terms = Terms(query);
        std::vector<FileNote const*> result;
        for (auto const& note : m_database.notes)
        {
            auto name = Lower(note.path.filename().wstring());
            auto description = Lower(note.description);
            bool matches = std::ranges::all_of(terms, [&](auto term)
            {
                if (!term.empty() && term.front() == L'#') term.erase(term.begin());
                if (name.contains(term) || description.contains(term)) return true;
                return std::ranges::any_of(note.tags, [&](auto const& tag) { return Lower(tag).contains(term); });
            });
            if (matches) result.push_back(&note);
        }
        std::ranges::sort(result, [](auto left, auto right) { return left->updatedAt > right->updatedAt; });
        return result;
    }

    FileNote& AppStore::GetOrCreateLocked(std::filesystem::path const& path)
    {
        if (auto note = Get(path)) return *note;
        FileNote note;
        note.path = Normalize(path);
        note.updatedAt = NowText();
        m_database.notes.push_back(std::move(note));
        return m_database.notes.back();
    }

    void AppStore::MarkStarted(FileNote& note)
    {
        if (!EqualText(note.status, L"New") || !note.newStatusAvailable) return;
        note.status = L"Ongoing";
        note.newStatusAvailable = false;
    }

    void AppStore::SaveNote(std::filesystem::path const& path, std::wstring description,
        std::vector<std::wstring> tags, std::optional<std::wstring> groupId)
    {
        std::scoped_lock guard(m_mutex);
        auto& note = GetOrCreateLocked(path);
        note.description = Trim(std::move(description));
        auto automatic = std::ranges::find_if(note.tags, IsAutomaticTag);
        auto existingRole = automatic != note.tags.end() ? *automatic : std::wstring{};
        note.tags.clear();
        for (auto& tag : tags)
        {
            tag = Trim(std::move(tag));
            while (!tag.empty() && tag.front() == L'#') tag.erase(tag.begin());
            if (tag.empty() || IsReservedTag(tag) || std::ranges::any_of(note.tags, [&](auto const& value) { return EqualText(value, tag); })) continue;
            note.tags.push_back(tag);
            if (!std::ranges::any_of(m_database.tagDefinitions, [&](auto const& value) { return EqualText(value, tag); }))
                m_database.tagDefinitions.push_back(tag);
        }
        if (!existingRole.empty()) note.tags.push_back(std::move(existingRole));
        note.ownMetadata = true;
        if (!GroupInherited(path)) note.groupId = groupId && !groupId->empty() ? groupId : std::nullopt;
        if (!note.ownStatus && !GroupInherited(path) && (!note.description.empty() || !note.tags.empty() || note.groupId)) MarkStarted(note);
        note.updatedAt = NowText();
        if (std::filesystem::is_directory(path)) note.cascadeMetadata = note.cascadeGroup = true;
        Reconcile(path, true, true);
        PersistLocked();
    }

    void AppStore::SetGroup(std::span<std::filesystem::path const> paths, std::optional<std::wstring> groupId)
    {
        std::scoped_lock guard(m_mutex);
        for (auto const& path : paths)
        {
            if (GroupInherited(path)) { Reconcile(path,true,true); continue; }
            auto& note = GetOrCreateLocked(path);
            if (note.groupId!=groupId) note.ownMetadata=true;
            note.groupId = groupId && !groupId->empty() ? groupId : std::nullopt;
            if (note.groupId && !note.ownStatus) MarkStarted(note);
            note.updatedAt = NowText();
            if (std::filesystem::is_directory(path)) note.cascadeGroup = true;
            Reconcile(path, true, true);
        }
        PersistLocked();
    }

    void AppStore::SetStatus(std::span<std::filesystem::path const> paths, std::wstring const& status)
    {
        std::scoped_lock guard(m_mutex);
        for (auto const& path : paths)
        {
            auto& note = GetOrCreateLocked(path);
            note.status = status;
            note.newStatusAvailable = false;
            note.updatedAt = NowText();
            note.ownStatus = true;
            if (std::filesystem::is_directory(path)) note.cascadeStatus = true;
            Reconcile(path, true, true);
        }
        PersistLocked();
    }

    void AppStore::SetReminder(std::span<std::filesystem::path const> paths, std::optional<std::wstring> reminderAt)
    {
        std::scoped_lock guard(m_mutex);
        for (auto const& path : paths)
        {
            auto& note = GetOrCreateLocked(path);
            note.reminderAt = reminderAt;
            note.updatedAt = NowText();
        }
        PersistLocked();
    }

    std::vector<PanelNote> const& AppStore::PanelNotes() const noexcept { return m_database.panelNotes; }

    void AppStore::SavePanelNote(PanelNote note)
    {
        std::scoped_lock guard(m_mutex);
        note.title = Trim(std::move(note.title));
        if (note.title.empty()) throw hresult_invalid_argument(L"Enter a title for this note.");
        auto& entries = m_database.panelNotes;
        auto previous = entries;
        if (note.id.empty())
        {
            note.id = NewId();
            entries.insert(entries.begin(), std::move(note));
        }
        else
        {
            auto it = std::ranges::find(entries, note.id, &PanelNote::id);
            if (it == entries.end()) throw hresult_invalid_argument(L"This note no longer exists.");
            *it = std::move(note);
        }
        try { PersistLocked(); }
        catch (...) { entries = std::move(previous); throw; }
    }

    void AppStore::DeletePanelNote(std::wstring const& id)
    {
        std::scoped_lock guard(m_mutex);
        auto previous = m_database.panelNotes;
        std::erase_if(m_database.panelNotes, [&](PanelNote const& note) { return note.id == id; });
        try { PersistLocked(); }
        catch (...) { m_database.panelNotes = std::move(previous); throw; }
    }

    void AppStore::SetPinned(std::span<std::filesystem::path const> paths, bool pinned)
    {
        std::scoped_lock guard(m_mutex);
        for (auto const& path : paths) GetOrCreateLocked(path).pinned = pinned;
        PersistLocked();
    }

    FileGroup const& AppStore::AddGroup(std::wstring name, std::wstring color)
    {
        std::scoped_lock guard(m_mutex);
        name = Trim(std::move(name));
        if (name.empty()) throw hresult_invalid_argument(L"Group name is required.");
        auto found = std::ranges::find_if(m_database.groups, [&](auto const& group) { return EqualText(group.name, name); });
        if (found != m_database.groups.end()) return *found;
        m_database.groups.push_back({ NewId(), std::move(name), std::move(color) });
        PersistLocked();
        return m_database.groups.back();
    }

    void AppStore::UpdateGroup(std::wstring const& id, std::wstring name, std::wstring color)
    {
        std::scoped_lock guard(m_mutex);
        name = Trim(std::move(name));
        if (name.empty()) throw hresult_invalid_argument(L"Group name is required.");
        auto& groups = m_database.groups;
        if (std::ranges::any_of(groups, [&](auto const& group) { return group.id != id && EqualText(group.name, name); }))
            throw hresult_invalid_argument(L"A group already uses that name.");
        auto group = std::ranges::find(groups, id, &FileGroup::id);
        if (group == groups.end()) throw hresult_invalid_argument(L"Group no longer exists.");
        group->name = std::move(name);
        group->color = std::move(color);
        PersistLocked();
    }

    static std::wstring TagName(std::wstring name)
    {
        name = Trim(std::move(name));
        while (!name.empty() && name.front() == L'#') name.erase(name.begin());
        name = Trim(std::move(name));
        if (name.empty()) throw hresult_invalid_argument(L"Tag name is required.");
        if (IsReservedTag(name)) throw hresult_invalid_argument(L"That tag is reserved for automatic folder and file tags.");
        return name;
    }

    std::wstring const& AppStore::AddTag(std::wstring name)
    {
        std::scoped_lock guard(m_mutex);
        name = TagName(std::move(name));
        auto found = std::ranges::find_if(m_database.tagDefinitions, [&](auto const& value) { return EqualText(value, name); });
        if (found != m_database.tagDefinitions.end()) return *found;
        auto automatic = std::ranges::find_if(m_database.tagDefinitions, IsAutomaticTag);
        auto inserted = m_database.tagDefinitions.insert(automatic, std::move(name));
        PersistLocked();
        return *inserted;
    }

    std::wstring AppStore::RenameTag(std::wstring const& oldName, std::wstring name)
    {
        std::scoped_lock guard(m_mutex);
        TagName(oldName);
        name = TagName(std::move(name));
        auto& tags = m_database.tagDefinitions;
        auto current = std::ranges::find_if(tags, [&](auto const& tag) { return EqualText(tag, oldName); });
        if (current == tags.end()) throw hresult_invalid_argument(L"Tag no longer exists.");
        if (std::ranges::any_of(tags, [&](auto const& tag) { return &tag != &*current && EqualText(tag, name); }))
            throw hresult_invalid_argument(L"A tag already uses that name.");
        for (auto& note : m_database.notes)
        {
            for (auto& tag : note.tags) if (EqualText(tag, oldName)) tag = name;
            for (auto& tag : note.parentTags) if (EqualText(tag, oldName)) tag = name;
        }
        *current = name;
        PersistLocked();
        return name;
    }

    void AppStore::ReorderTag(std::wstring const& name, std::wstring const& target)
    {
        std::scoped_lock guard(m_mutex);
        if (IsReservedTag(name) || IsReservedTag(target)) return;
        auto& tags = m_database.tagDefinitions;
        auto from = std::ranges::find(tags, name);
        auto to = std::ranges::find(tags, target);
        if (from == tags.end() || to == tags.end() || from == to) return;
        if (from < to) std::rotate(from, from + 1, to + 1);
        else std::rotate(to, from, from + 1);
        PersistLocked();
    }

    bool AppStore::GroupInUse(std::wstring const& id) const noexcept
    {
        return std::ranges::any_of(m_database.notes, [&](auto const& note)
        {
            return note.groupId == id || note.archiveGroupId == id;
        });
    }

    bool AppStore::DeleteGroup(std::wstring const& id)
    {
        std::scoped_lock guard(m_mutex);
        if (GroupInUse(id)) return false;
        auto removed = std::erase_if(m_database.groups, [&](auto const& group) { return group.id == id; });
        if (removed) PersistLocked();
        return removed != 0;
    }

    bool AppStore::DeleteTag(std::wstring const& name)
    {
        std::scoped_lock guard(m_mutex);
        if (IsReservedTag(name)) return false;
        auto removed = std::erase_if(m_database.tagDefinitions, [&](auto const& tag) { return EqualText(tag, name); });
        bool changed = removed != 0;
        for (auto& note : m_database.notes)
        {
            auto forgotten=std::erase_if(note.parentTags,[&](auto const& tag){return EqualText(tag,name);});
            auto count = forgotten + std::erase_if(note.tags, [&](auto const& tag) { return EqualText(tag, name); });
            if (count)
            {
                note.updatedAt = NowText();
                changed = true;
            }
        }
        if (changed) PersistLocked();
        return changed;
    }

    void AppStore::MovePath(std::filesystem::path const& oldPath, std::filesystem::path const& newPath)
    {
        std::array paths{ std::pair{ oldPath, newPath } };
        MovePaths(paths);
    }

    void AppStore::MovePaths(std::span<std::pair<std::filesystem::path, std::filesystem::path> const> paths)
    {
        std::scoped_lock guard(m_mutex);
        bool changed{};
        for (auto& entry : m_database.panelNotes)
            for (auto& file : entry.files)
                for (auto const& [oldPath, newPath] : paths)
                    if (auto relative = RelativeInside(file, oldPath))
                    {
                        file = Normalize(relative->empty() ? newPath : newPath / *relative);
                        changed = true;
                        break;
                    }
        for (auto& note : m_database.notes)
        {
            for (auto const& [oldPath, newPath] : paths)
            {
                auto relative = RelativeInside(note.path, oldPath);
                if (!relative) continue;
                note.path = Normalize(relative->empty() ? newPath : newPath / *relative);
                if (note.legacyArchiveSource && !RelativeInside(note.path, m_database.legacyArchiveRoot)) note.legacyArchiveSource = false;
                note.updatedAt = NowText();
                changed = true;
                break;
            }
        }
        for (auto const& [oldPath, newPath] : paths)
        {
            if (oldPath.parent_path() == newPath.parent_path()) continue;
            if (auto moved = Get(newPath)) moved->parentTags.clear();
            changed |= Reconcile(newPath, true, true);
        }
        if (changed) PersistLocked();
    }

    void AppStore::DeletePath(std::filesystem::path const& path)
    {
        std::array paths{ path };
        DeletePaths(paths);
    }

    void AppStore::DeletePaths(std::span<std::filesystem::path const> paths)
    {
        std::scoped_lock guard(m_mutex);
        auto removed = std::erase_if(m_database.notes, [&](auto const& note)
        {
            return std::ranges::any_of(paths, [&](auto const& path) { return RelativeInside(note.path, path).has_value(); });
        });
        for (auto& entry : m_database.panelNotes)
            removed += std::erase_if(entry.files, [&](auto const& file)
            {
                return std::ranges::any_of(paths, [&](auto const& path) { return RelativeInside(file, path).has_value(); });
            });
        if (removed) PersistLocked();
    }

    void AppStore::ReplaceWithArchive(std::span<std::filesystem::path const> sourcePaths,
        std::filesystem::path const& archivePath, std::optional<std::wstring> groupId)
    {
        std::scoped_lock guard(m_mutex);
        std::vector<FileNote> sourceNotes;
        for (auto const& path : sourcePaths)
        {
            if (auto note = Get(path)) sourceNotes.push_back(*note);
        }
        std::erase_if(m_database.notes, [&](auto const& note)
        {
            return std::ranges::any_of(sourcePaths, [&](auto const& path) { return RelativeInside(note.path, path).has_value(); });
        });
        if (sourceNotes.size() == 1)
        {
            auto note = std::move(sourceNotes.front());
            note.path = Normalize(archivePath);
            note.groupId = groupId;
            note.status = L"Done";
            note.newStatusAvailable = false;
            note.updatedAt = NowText();
            note.cascadeMetadata = note.cascadeGroup = note.cascadeStatus = false;
            SetAutomaticTag(note);
            m_database.notes.push_back(std::move(note));
        }
        else if (!sourceNotes.empty() || groupId)
        {
            FileNote merged;
            merged.path = Normalize(archivePath);
            merged.groupId = groupId;
            merged.status = L"Done";
            merged.newStatusAvailable = false;
            merged.updatedAt = NowText();
            for (auto const& source : sourceNotes)
            {
                if (!source.description.empty())
                {
                    if (!merged.description.empty()) merged.description += L"\r\n\r\n";
                    merged.description += source.path.filename().wstring() + L": " + source.description;
                }
                for (auto const& tag : source.tags)
                    if (!IsReservedTag(tag) && !std::ranges::any_of(merged.tags, [&](auto const& value) { return EqualText(value, tag); }))
                        merged.tags.push_back(tag);
                if (source.reminderAt && (!merged.reminderAt || *source.reminderAt < *merged.reminderAt)) merged.reminderAt = source.reminderAt;
            }
            m_database.notes.push_back(std::move(merged));
        }
        PersistLocked();
    }

    bool AppStore::GroupInherited(std::filesystem::path const& path) const
    {
        std::scoped_lock guard(m_mutex);
        for (auto p = path.parent_path(); !p.empty(); p = p.parent_path())
        {
            if (auto n = Get(p); n && (n->cascadeMetadata || n->cascadeGroup)) return true;
            if (p == p.root_path()) break;
        }
        return false;
    }

    bool AppStore::Reconcile(std::filesystem::path const& path, bool includeRoot, bool recursive)
    {
        struct Inheritance
        {
            std::optional<size_t> metadata, group, status;
            bool active() const { return metadata || group || status; }
        };
        using fs = std::filesystem::path;
        std::unordered_map<std::wstring, size_t> index;
        for (size_t i = 0; i < m_database.notes.size(); ++i)
            index.emplace(Lower(m_database.notes[i].path.lexically_normal().wstring()), i);
        auto extend = [&](Inheritance state, size_t id)
        {
            auto const& n = m_database.notes[id];
            if (n.cascadeMetadata) state.metadata = id;
            if (!state.group && (n.cascadeGroup || n.cascadeMetadata)) state.group = id;
            if (n.cascadeStatus || (!state.status && (n.cascadeMetadata || n.cascadeGroup))) state.status = id;
            return state;
        };
        auto root = Normalize(path);
        std::vector<size_t> ancestors;
        for (auto p = root.parent_path(); !p.empty() && p != root; p = p.parent_path())
        {
            if (auto it=index.find(Lower(p.wstring())); it!=index.end()) ancestors.push_back(it->second);
            if (p == p.root_path()) break;
        }
        Inheritance state;
        for (auto it=ancestors.rbegin(); it!=ancestors.rend(); ++it) state=extend(state,*it);
        bool changed{};
        auto apply = [&](fs const& entry, bool directory, Inheritance parent)
        {
            auto key=Lower(entry.wstring());
            auto found=index.find(key);
            if (found==index.end() && !parent.active()) return parent;
            if (found==index.end())
            {
                FileNote added; added.path=entry;
                found=index.emplace(key,m_database.notes.size()).first;
                m_database.notes.push_back(std::move(added));
                changed=true;
            }
            auto& n=m_database.notes[found->second];
            auto tags=n.tags;
            auto group=n.groupId;
            auto status=n.status;
            auto available=n.newStatusAvailable;
            auto description=n.description;
            auto offered=n.parentTags;
            SetAutomaticTag(n);
            if (parent.metadata)
            {
                auto const& source=m_database.notes[*parent.metadata];
                std::vector<std::wstring> incoming;
                for (auto const& tag : source.tags) if (!IsReservedTag(tag)) incoming.push_back(tag);
                if (!n.ownMetadata) { n.description=source.description; n.tags=incoming; }
                else for (auto const& tag : incoming)
                {
                    if (!ContainsTag(n.parentTags,tag) && !ContainsTag(n.tags,tag)) n.tags.push_back(tag);
                }
                n.parentTags=std::move(incoming);
            }
            else n.parentTags.clear();
            if (parent.group) n.groupId=m_database.notes[*parent.group].groupId;
            if (parent.status && !n.ownStatus)
            {
                n.status=m_database.notes[*parent.status].status;
                n.newStatusAvailable=m_database.notes[*parent.status].newStatusAvailable;
            }
            if (parent.group || parent.metadata) n.tags.emplace_back(directory ? subfolderTag : subfileTag);
            else if (directory && (n.cascadeMetadata || n.cascadeGroup)) n.tags.emplace_back(folderTag);
            if (tags!=n.tags || group!=n.groupId || status!=n.status || available!=n.newStatusAvailable ||
                description!=n.description || offered!=n.parentTags)
            { n.updatedAt=NowText(); changed=true; }
            return directory ? extend(parent,found->second) : parent;
        };
        auto attrs=GetFileAttributesW(root.c_str());
        if (attrs==INVALID_FILE_ATTRIBUTES || (attrs&FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        bool directory=(attrs&FILE_ATTRIBUTE_DIRECTORY)!=0;
        if (includeRoot) state=apply(root,directory,state);
        else if (auto it=index.find(Lower(root.wstring())); it!=index.end()) state=extend(state,it->second);
        if (!directory || (!state.active() && !includeRoot)) return changed;
        std::vector<Inheritance> levels{state};
        std::error_code error;
        auto options=std::filesystem::directory_options::skip_permission_denied;
        for (std::filesystem::recursive_directory_iterator it(root,options,error),end; it!=end; it.increment(error))
        {
            if (error) break;
            auto attributes=GetFileAttributesW(it->path().c_str());
            if (attributes==INVALID_FILE_ATTRIBUTES || (attributes&FILE_ATTRIBUTE_REPARSE_POINT))
            { it.disable_recursion_pending(); continue; }
            levels.resize(static_cast<size_t>(it.depth())+1);
            auto next=apply(it->path(),(attributes&FILE_ATTRIBUTE_DIRECTORY)!=0,levels.back());
            if (!recursive) it.disable_recursion_pending();
            else levels.push_back(next);
        }
        return changed;
    }

    void AppStore::InheritNewChildren(std::filesystem::path const& folder)
    {
        std::scoped_lock guard(m_mutex);
        if (Reconcile(folder, false, false)) PersistLocked();
    }

    void AppStore::RegisterArchiveGroup(std::filesystem::path const& folder, std::wstring const& groupId)
    {
        std::scoped_lock guard(m_mutex);
        auto& note = GetOrCreateLocked(folder);
        if (note.archiveGroupId == groupId) return;
        note.archiveGroupId = groupId;
        if (!note.groupId) note.groupId = groupId;
        note.updatedAt = NowText();
        PersistLocked();
    }

    void AppStore::Save()
    {
        std::scoped_lock guard(m_mutex);
        PersistLocked();
    }

    void AppStore::BackupTo(std::filesystem::path const& path, std::wstring_view backupPassword) const
    {
        if (EqualText(Normalize(path).wstring(), Normalize(m_databasePath).wstring()))
            throw hresult_error(E_INVALIDARG, L"Choose a backup file separate from Hudes's live database.");
        auto key = MakeDatabaseKey(backupPassword);
        std::scoped_lock guard(m_mutex);
        WriteDatabaseFile(path, SerializeDatabase(m_database), key.get());
    }

    void AppStore::RestoreFrom(std::filesystem::path const& path, std::wstring_view backupPassword)
    {
        auto candidate = ReadDatabase(DecodeDatabaseFile(path, backupPassword).json);
        if (candidate.version < 1) throw hresult_error(E_INVALIDARG, L"Unsupported Hudes database version.");
        std::scoped_lock guard(m_mutex);
        auto previous = std::exchange(m_database, std::move(candidate));
        try
        {
            EnsureDefaults();
            PersistLocked();
        }
        catch (...)
        {
            m_database = std::move(previous);
            throw;
        }
    }

    bool AppStore::IsDatabaseProtected() const noexcept
    {
        return m_databaseKey != nullptr;
    }

    void AppStore::SetDatabasePassword(std::wstring_view password)
    {
        auto key = MakeDatabaseKey(password);
        std::scoped_lock guard(m_mutex);
        WriteDatabaseFile(m_databasePath, SerializeDatabase(m_database), key.get());
        m_databaseKey = std::move(key);
    }

    bool AppStore::VerifyPassword(std::wstring const& password) const
    {
        try
        {
            auto salt = FromBase64(m_database.settings.passwordSalt);
            auto expected = FromBase64(m_database.settings.passwordHash);
            auto actual = PasswordHash(password, salt);
            return FixedEqual(expected, actual);
        }
        catch (...) { return false; }
    }

    void AppStore::SetPassword(std::wstring const& password)
    {
        std::scoped_lock guard(m_mutex);
        std::vector<std::uint8_t> salt(16);
        check_nt(BCryptGenRandom(nullptr, salt.data(), static_cast<ULONG>(salt.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG));
        m_database.settings.passwordSalt = Base64(salt);
        m_database.settings.passwordHash = Base64(PasswordHash(password, salt));
    }

    void AppStore::LoadOrCreate(std::wstring_view password, std::optional<std::wstring_view> migrationPassword)
    {
        std::scoped_lock guard(m_mutex);
        if (std::filesystem::exists(m_databasePath))
        {
            auto decoded = DecodeDatabaseFile(m_databasePath, password);
            m_database = ReadDatabase(decoded.json);
            if (decoded.windowsBound)
            {
                if (!migrationPassword) throw DatabaseMigrationChoiceRequired();
                m_databaseKey = MakeDatabaseKey(*migrationPassword);
            }
            else m_databaseKey = std::move(decoded.key);
            if (decoded.legacy && m_database.version < 3)
            {
                auto backup = m_databasePath; backup += L".pre-v4.1.1.bak.json";
                if (!std::filesystem::exists(backup)) WriteDatabaseFile(backup, decoded.json, m_databaseKey.get());
            }
            if (m_database.version < 5 && m_database.settings.desktopFolderConfirmed &&
                std::filesystem::is_directory(m_database.settings.desktopFolder / L"archive"))
            {
                auto backup = m_databasePath; backup += L".pre-4.5.3.bak.hudesdb";
                if (!std::filesystem::exists(backup)) WriteDatabaseFile(backup, decoded.json, m_databaseKey.get());
            }
        }
        EnsureDefaults();
        PersistLocked();
    }

    void AppStore::EnsureDefaults()
    {
        if (m_database.version < 4)
        {
            UpgradeAutomaticTags(m_database);
            m_database.version = 4;
        }
        if (m_database.version < 5)
        {
            ImportLegacyArchive();
            m_database.version = 5;
        }
        if (m_database.version < 6)
        {
            // Earlier records stored the effective values. Recover explicit edits where values differ.
            std::unordered_map<std::wstring, FileNote const*> legacy;
            for (auto const& n:m_database.notes) legacy.emplace(Lower(n.path.lexically_normal().wstring()),&n);
            for (auto& n : m_database.notes)
            {
                FileNote const* metadata{};
                FileNote const* status{};
                FileNote const* group{};
                for (auto p=n.path.parent_path(); !p.empty(); p=p.parent_path())
                {
                    if (auto match=legacy.find(Lower(p.wstring())); match!=legacy.end())
                    {
                        auto a=match->second;
                        if (!metadata && a->cascadeMetadata) metadata=a;
                        if (!status && a->cascadeStatus) status=a;
                        if (a->cascadeMetadata || a->cascadeGroup) group=a;
                    }
                    if (p==p.root_path()) break;
                }
                auto custom=n.tags;
                std::erase_if(custom,IsReservedTag);
                if (metadata)
                    for (auto const& tag : metadata->tags) if (!IsReservedTag(tag)) n.parentTags.push_back(tag);
                n.ownMetadata=n.cascadeMetadata || (metadata ? n.description!=metadata->description || custom!=n.parentTags :
                    !n.description.empty() || !custom.empty() || n.groupId.has_value());
                auto baseline=status ? status : (metadata ? metadata : group);
                n.ownStatus=n.cascadeStatus || (baseline ? n.status!=baseline->status :
                    !(n.status==L"New" && n.newStatusAvailable) && !(n.status==L"Ongoing" && (!custom.empty() || !n.description.empty() || n.groupId)));
                if (group)
                {
                    n.groupId=group->groupId;
                    bool directory=n.cascadeMetadata || n.cascadeGroup || n.cascadeStatus || ContainsTag(n.tags,subfolderTag) || ContainsTag(n.tags,folderTag);
                    SetAutomaticTag(n,directory ? subfolderTag : subfileTag);
                }
            }
            m_database.version=6;
        }
        std::erase_if(m_database.tagDefinitions, IsReservedTag);
        for (auto tag : automaticTags) m_database.tagDefinitions.emplace_back(tag);
        if (m_database.settings.passwordHash.empty()) SetPassword(L"1111");
        for (auto& note : m_database.notes)
        {
            std::erase_if(note.tags, [](auto const& tag) { return EqualText(tag, L"SUB"); });
            if (note.status.empty()) note.status = L"New";
            for (auto const& tag : note.tags)
                if (!std::ranges::any_of(m_database.tagDefinitions, [&](auto const& value) { return EqualText(value, tag); })) m_database.tagDefinitions.push_back(tag);
            if (EqualText(note.status, L"New") && note.newStatusAvailable && (!note.description.empty() || !note.tags.empty() || note.groupId)) MarkStarted(note);
            else if (!EqualText(note.status, L"New")) note.newStatusAvailable = false;
        }
    }

    void AppStore::ImportLegacyArchive()
    {
        if (!m_database.settings.desktopFolderConfirmed || m_database.settings.desktopFolder.empty()) return;
        auto root = m_database.settings.desktopFolder / L"archive";
        auto attrs = GetFileAttributesW(root.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY) || (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) return;
        m_database.legacyArchiveRoot = Normalize(root);
        std::error_code error;
        auto options = std::filesystem::directory_options::skip_permission_denied;
        auto convert = [&](std::filesystem::path const& path, std::optional<std::wstring> const& group)
        {
            auto flags = GetFileAttributesW(path.c_str());
            if (flags == INVALID_FILE_ATTRIBUTES || flags & FILE_ATTRIBUTE_REPARSE_POINT || EqualText(path.filename().wstring(), L"desktop.ini")) return;
            auto existing = Get(path);
            auto& note = existing ? *existing : GetOrCreateLocked(path);
            note.archived = true;
            note.legacyArchiveSource = true;
            if (!note.groupId && group) note.groupId = group;
            if (!existing) { note.status = L"Done"; note.newStatusAvailable = false; }
        };
        for (std::filesystem::directory_iterator entry(root, options, error), end; entry != end && !error; entry.increment(error))
        {
            auto path = entry->path();
            auto flags = GetFileAttributesW(path.c_str());
            if (flags == INVALID_FILE_ATTRIBUTES || (flags & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
            std::optional<std::wstring> owner;
            if (flags & FILE_ATTRIBUTE_DIRECTORY)
            {
                if (auto note = Get(path)) owner = note->archiveGroupId;
                if (!owner)
                {
                    for (auto const& group : m_database.groups)
                    {
                        auto name = group.name;
                        for (auto& ch : name) if (wcschr(L"<>:\"/\\|?*", ch)) ch = L'_';
                        if (EqualText(name, path.filename().wstring())) { owner = group.id; break; }
                    }
                }
            }
            if (!owner) { convert(path, {}); continue; }
            m_database.legacyArchiveGroups.push_back(Normalize(path));
            std::error_code childError;
            for (std::filesystem::directory_iterator child(path, options, childError), childEnd;
                child != childEnd && !childError; child.increment(childError)) convert(child->path(), owner);
            std::erase_if(m_database.notes, [&](auto const& note) { return SamePath(note.path, path); });
        }
        std::erase_if(m_database.notes, [&](auto const& note) { return SamePath(note.path, root); });
    }

    void AppStore::PersistLocked() const
    {
        WriteDatabaseFile(m_databasePath, SerializeDatabase(m_database), m_databaseKey.get());
    }
}

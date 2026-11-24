#include "pch.h"
#include "Workspace.h"
#include "NativeServices.h"
#include <algorithm>
#include <fstream>
#include <unordered_map>
#include <unordered_set>

using namespace winrt;
namespace fs = std::filesystem;

namespace
{
    std::wstring PathKey(fs::path const& path)
    {
        auto key = path.lexically_normal().wstring();
        std::ranges::transform(key, key.begin(), [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
        return key;
    }

    std::wstring SafeGroupName(std::wstring name)
    {
        for (auto& ch : name)
            if (ch < 32 || wcschr(L"<>:\"/\\|?*", ch)) ch = L'_';
        while (!name.empty() && (name.back() == L'.' || name.back() == L' ')) name.pop_back();
        if (name.empty()) return L"Group";
        try { HudesNative::ValidateItemName(name); }
        catch (hresult_invalid_argument const&) { name.insert(name.begin(), L'_'); }
        return name;
    }

    std::string Xml(std::wstring_view value)
    {
        std::wstring escaped;
        for (auto ch : value)
        {
            if (ch == L'&') escaped += L"&amp;";
            else if (ch == L'<') escaped += L"&lt;";
            else if (ch == L'>') escaped += L"&gt;";
            else if (ch == L'\r') escaped += L"&#13;";
            else if (ch >= 32 || ch == L'\t' || ch == L'\n') escaped += ch;
        }
        return to_string(hstring(escaped));
    }

    std::string Csv(std::wstring_view value)
    {
        std::wstring escaped = L"\"";
        auto first = value.find_first_not_of(L" \r\n\t");
        if (first != value.npos && wcschr(L"=+-@", value[first])) escaped += L"'";
        for (auto ch : value)
        {
            if (ch == L'"') escaped += L'"';
            escaped += ch;
        }
        escaped += L'"';
        return to_string(hstring(escaped));
    }

    void WritePart(fs::path const& path, char const* text)
    {
        std::ofstream output(path, std::ios::binary);
        output.exceptions(std::ios::badbit | std::ios::failbit);
        output << text;
        output.close();
    }

    class MetadataWriter
    {
    public:
        MetadataWriter(fs::path root, bool xlsx, std::uint32_t limit)
            : m_root(std::move(root)), m_xlsx(xlsx), m_limit(limit)
        {
            fs::create_directory(m_root);
        }

        void Add(fs::path const& key, HudesNative::FileNote const* note)
        {
            if (!m_stream.is_open()) StartPart();
            if (m_rowsInPart == m_limit) { FinishPart(); StartPart(); }
            std::wstring tags;
            if (note)
                for (auto const& tag : note->tags) { if (!tags.empty()) tags += L"; "; tags += tag; }
            Row(key.generic_wstring(), note ? note->description : L"", tags);
            ++m_rowsInPart;
            ++rows;
        }

        void Finish()
        {
            if (!m_stream.is_open() && !parts) StartPart();
            if (m_stream.is_open()) FinishPart();
        }

        std::size_t rows{}, parts{};
    private:
        fs::path m_root, m_package;
        bool m_xlsx{};
        std::uint32_t m_limit{}, m_rowsInPart{};
        std::ofstream m_stream;

        void Row(std::wstring_view path, std::wstring_view note, std::wstring_view tags)
        {
            if (!m_xlsx)
            {
                m_stream << Csv(path) << ',' << Csv(note) << ',' << Csv(tags) << "\r\n";
                return;
            }
            auto cell = [&](std::wstring_view value)
            {
                m_stream << "<c t=\"inlineStr\"><is><t xml:space=\"preserve\">" << Xml(value) << "</t></is></c>";
            };
            auto prefix = [](std::wstring_view value)
            {
                auto length = (std::min)(value.size(), std::size_t{32767});
                if (length < value.size() && value[length - 1] >= 0xd800 && value[length - 1] <= 0xdbff) --length;
                return value.substr(0, length);
            };
            m_stream << "<row>";
            cell(path);
            auto firstNote = prefix(note), firstTags = prefix(tags);
            cell(firstNote); cell(firstTags);
            note.remove_prefix(firstNote.size()); tags.remove_prefix(firstTags.size());
            auto continued = [&](std::wstring_view rest, wchar_t const* label)
            {
                while (!rest.empty())
                {
                    auto chunk = prefix(rest.substr(0, 32730));
                    if (chunk.size() < rest.size() && chunk.back() >= 0xd800 && chunk.back() <= 0xdbff) chunk.remove_suffix(1);
                    cell(std::wstring(label) + std::wstring(chunk));
                    rest.remove_prefix(chunk.size());
                }
            };
            continued(note, L"Note continued: ");
            continued(tags, L"Tags continued: ");
            m_stream << "</row>\n";
        }

        void StartPart()
        {
            ++parts;
            m_rowsInPart = 0;
            m_stream.exceptions(std::ios::badbit | std::ios::failbit);
            if (m_xlsx)
            {
                m_package = m_root / L".xlsx-parts";
                fs::create_directories(m_package / L"_rels");
                fs::create_directories(m_package / L"xl" / L"_rels");
                fs::create_directories(m_package / L"xl" / L"worksheets");
                m_stream.open(m_package / L"xl" / L"worksheets" / L"sheet1.xml", std::ios::binary);
                m_stream << "<?xml version=\"1.0\" encoding=\"UTF-8\"?><worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\"><sheetData>\n";
            }
            else
            {
                m_stream.open(m_root / (L"Metadata " + std::to_wstring(parts) + L".csv"), std::ios::binary);
                m_stream << "\xEF\xBB\xBF";
            }
            Row(L"Path", L"Note", L"Tags");
        }

        void FinishPart()
        {
            if (m_xlsx) m_stream << "</sheetData></worksheet>";
            m_stream.close();
            if (!m_xlsx) return;
            WritePart(m_package / L"[Content_Types].xml",
                R"(<?xml version="1.0" encoding="UTF-8"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="xml" ContentType="application/xml"/><Override PartName="/xl/workbook.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/><Override PartName="/xl/worksheets/sheet1.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/></Types>)");
            WritePart(m_package / L"_rels" / L".rels",
                R"(<?xml version="1.0" encoding="UTF-8"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="xl/workbook.xml"/></Relationships>)");
            WritePart(m_package / L"xl" / L"workbook.xml",
                R"(<?xml version="1.0" encoding="UTF-8"?><workbook xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"><sheets><sheet name="Metadata" sheetId="1" r:id="rId1"/></sheets></workbook>)");
            WritePart(m_package / L"xl" / L"_rels" / L"workbook.xml.rels",
                R"(<?xml version="1.0" encoding="UTF-8"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet1.xml"/></Relationships>)");
            std::array sources{ m_package / L"[Content_Types].xml", m_package / L"_rels", m_package / L"xl" };
            HudesNative::CreateZip(sources, m_root / (L"Metadata " + std::to_wstring(parts) + L".xlsx"));
            fs::remove_all(m_package);
        }
    };
}

namespace HudesNative
{
    fs::path const& ArchiveRoot()
    {
        static const fs::path path(L":hudes-archive:");
        return path;
    }

    fs::path ArchiveGroupPath(std::optional<std::wstring> const& groupId)
    {
        if (!groupId) return ArchiveRoot() / L"ungrouped";
        std::wstring name = L"g";
        for (auto ch : *groupId)
        {
            constexpr wchar_t hex[] = L"0123456789abcdef";
            auto value = static_cast<unsigned>(ch);
            for (int shift = 12; shift >= 0; shift -= 4) name += hex[(value >> shift) & 15];
        }
        return ArchiveRoot() / name;
    }

    bool HiddenInFileView(fs::path path, fs::path const& stopAt)
    {
        while (!path.empty() && path!=path.root_path() && (stopAt.empty() || !SamePath(path,stopAt)))
        {
            auto name=path.filename().wstring();
            if(!name.empty() && (name.front()==L'.' || name.front()==L'~')) return true;
            auto attr=GetFileAttributesW(path.c_str());
            if(attr!=INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_HIDDEN)) return true;
            if(stopAt.empty()) break;
            path=path.parent_path();
        }
        return false;
    }

    bool IsVirtualArchive(fs::path const& path)
    {
        return path == ArchiveRoot() || path.parent_path() == ArchiveRoot();
    }

    std::wstring NextFileName(fs::path const& folder, std::wstring const& base, std::wstring const& extension)
    {
        auto prefix = PathKey(fs::path(base + L"["));
        std::unordered_set<std::wstring> used;
        for (auto const& entry : fs::directory_iterator(folder))
        {
            auto name = PathKey(entry.path().filename());
            if (!name.starts_with(prefix)) continue;
            auto end = name.find(L']', prefix.size());
            if (end != name.npos && (end + 1 == name.size() || name[end + 1] == L'.'))
                used.insert(name.substr(0, end + 1));
        }
        for (std::uint64_t n = 1;; ++n)
        {
            auto stem = base + L"[" + std::to_wstring(n) + L"]";
            if (!used.contains(PathKey(fs::path(stem)))) return stem + extension;
        }
    }

    WorkspaceExportResult ExportWorkspace(fs::path const& workspace, fs::path const& legacyRoot,
        fs::path const& destination, std::span<FileGroup const> groups, std::span<FileNote const> notes,
        bool xlsx, std::uint32_t recordLimit, fs::path const& selection)
    {
        if (workspace.empty() || !fs::is_directory(workspace) || !fs::is_directory(destination))
            throw hresult_invalid_argument(L"Select an existing workspace and export destination.");
        if (SamePath(destination, workspace) || IsWithin(destination, workspace))
            throw hresult_invalid_argument(L"Choose a destination outside the Hudes Desktop folder.");
        if (!recordLimit || recordLimit > 1048575)
            throw hresult_invalid_argument(L"Set the records per file between 1 and 1,048,575.");

        const bool full = selection.empty();
        const bool entireArchive = selection == ArchiveRoot();
        auto label = full ? std::wstring(L"Hudes Export") : entireArchive ? L"Archive" : L"unGrouped";
        for (auto const& group : groups)
            if (selection == ArchiveGroupPath(group.id)) label = SafeGroupName(group.name);
        auto final = UniqueDestination(destination / label);
        GUID guid{};
        check_hresult(CoCreateGuid(&guid));
        wchar_t token[40]{};
        StringFromGUID2(guid, token, 40);
        auto pending = destination / (L".hudes-export-" + std::wstring(token));
        if (!fs::create_directory(pending)) throw hresult_error(E_FAIL, L"Could not create the export folder.");
        auto cleanup = wil::scope_exit([&] { std::error_code error; fs::remove_all(pending, error); });

        WorkspaceExportResult result;
        std::unordered_map<std::wstring, FileNote const*> noteIndex;
        for (auto const& note : notes) noteIndex.emplace(PathKey(note.path), &note);
        std::unique_ptr<MetadataWriter> metadata;
        if (full) metadata = std::make_unique<MetadataWriter>(pending / L"Hudes metadata", xlsx, recordLimit);
        auto archive = full ? pending / L"Archive" : pending;
        if (full) fs::create_directory(archive);
        std::unordered_map<fs::path, fs::path> groupFolders;
        auto ungrouped = ArchiveGroupPath({});
        if (full || entireArchive)
        {
            auto ungroupedPath = archive / L"unGrouped";
            fs::create_directory(ungroupedPath);
            groupFolders.emplace(ungrouped, ungroupedPath);
            for (auto const& group : groups)
            {
                auto path = UniqueDestination(archive / SafeGroupName(group.name));
                fs::create_directory(path);
                groupFolders.emplace(ArchiveGroupPath(group.id), path);
            }
        }
        else groupFolders.emplace(selection, pending);

        auto copy = [&](auto&& self, fs::path const& src, fs::path const& target, bool root) -> void
        {
            auto flags = GetFileAttributesW(src.c_str());
            if (flags == INVALID_FILE_ATTRIBUTES || (flags & FILE_ATTRIBUTE_REPARSE_POINT)) { ++result.skipped; return; }
            if (!legacyRoot.empty() && SamePath(src, legacyRoot)) return;
            if (_wcsicmp(src.filename().c_str(), L"desktop.ini") == 0 || _wcsicmp(src.filename().c_str(), L"hudes.db.json") == 0 || _wcsicmp(src.filename().c_str(), L"salif.db.json") == 0) return;
            auto found = noteIndex.find(PathKey(src));
            auto note = found == noteIndex.end() ? nullptr : found->second;
            if (!root && note && note->archived) return;
            auto path = UniqueDestination(target);
            if (flags & FILE_ATTRIBUTE_DIRECTORY)
            {
                fs::create_directory(path);
                if (metadata) metadata->Add(path.lexically_relative(pending), note);
                for (auto const& child : fs::directory_iterator(src)) self(self, child.path(), path / child.path().filename(), false);
            }
            else
            {
                fs::copy_file(src, path);
                if (metadata) metadata->Add(path.lexically_relative(pending), note);
            }
        };
        if (full)
            for (auto const& entry : fs::directory_iterator(workspace)) copy(copy, entry.path(), pending / entry.path().filename(), false);
        for (auto const& note : notes)
        {
            if (!IsWithin(note.path, workspace)) continue;
            if (!note.archived)
            {
                if (full && note.legacyArchiveSource) copy(copy, note.path, pending / note.path.filename(), true);
                continue;
            }
            auto id = note.groupId;
            if (id && std::ranges::none_of(groups, [&](auto const& group) { return group.id == *id; })) id.reset();
            if (auto target = groupFolders.find(ArchiveGroupPath(id)); target != groupFolders.end())
                copy(copy, note.path, target->second / note.path.filename(), true);
        }
        if (metadata)
        {
            metadata->Finish();
            result.records = metadata->rows;
            result.parts = metadata->parts;
        }
        check_bool(MoveFileExW(pending.c_str(), final.c_str(), 0));
        cleanup.release();
        result.folder = final;
        return result;
    }
}

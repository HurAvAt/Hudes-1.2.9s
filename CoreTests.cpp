#include "pch.h"
#include "CoreTests.h"
#include "AppStore.h"
#include "NativeServices.h"
#include "Workspace.h"
#include "Terminal.h"
#include <algorithm>
#include <array>
#include <fstream>
#include <wincrypt.h>
#include <winrt/Windows.Data.Json.h>

namespace HudesNative
{
    bool RunCoreTests()
    {
        GUID id{}; winrt::check_hresult(CoCreateGuid(&id));
        wchar_t identifier[40]{}; StringFromGUID2(id, identifier, 40);
        auto root = std::filesystem::temp_directory_path() / (L"hudes-v4-tests-" + std::wstring(identifier));
        // All generated files are constrained to this unique test directory.
        std::filesystem::create_directory(root);
        auto cleanup = wil::scope_exit([&] { std::error_code ignored; std::filesystem::remove_all(root, ignored); });
        std::ofstream report(ExecutableFolder() / L"self-test-results.txt", std::ios::trunc);
        auto require = [](bool valid, char const* message) { if (!valid) throw std::runtime_error(message); };
        auto hasTag = [](FileNote const* note, std::wstring const& tag)
        { return note && std::ranges::any_of(note->tags, [&](auto const& value) { return value == tag; }); };
        auto readBytes = [](std::filesystem::path const& path)
        {
            std::ifstream input(path, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
        };
        try
        {
            auto folder = root / L"parent";
            std::filesystem::create_directory(folder);
            auto nested = CreateEmptyItem(folder, L"nested", true);
            auto file = CreateEmptyItem(nested, L"Persian-فایل.txt", false);
            require(std::filesystem::is_directory(nested) && std::filesystem::file_size(file) == 0, "New folder or blank file creation failed");
            { std::ofstream output(file); output << "test"; }
            bool duplicateRejected{};
            try { CreateEmptyItem(nested, file.filename().wstring(), false); }
            catch (winrt::hresult_error const&) { duplicateRejected = true; }
            require(duplicateRejected && std::filesystem::file_size(file) == 4, "New file creation replaced an existing file");
            bool invalidRejected{};
            try { CreateEmptyItem(nested, L"..\\escaped.txt", false); }
            catch (winrt::hresult_invalid_argument const&) { invalidRejected = true; }
            require(invalidRejected && !std::filesystem::exists(folder / L"escaped.txt"), "New file creation escaped the current folder");
            require(CalculateFileHash(file, FileHashAlgorithm::Md5) == L"098f6bcd4621d373cade4e832627b4f6", "MD5 digest failed");
            require(CalculateFileHash(file, FileHashAlgorithm::Sha256) == L"9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08", "SHA-256 digest failed");
            auto db = root / L"test.json";
            AppStore store(db);
            require(store.Settings().themeMode == L"Light", "Fresh databases must start in light mode");
            auto firstId = store.AddGroup(L"First", L"#48AABB").id;
            auto secondId = store.AddGroup(L"Second", L"#BF8844").id;
            store.SaveNote(folder, L"Parent description", {L"Alpha", L"Beta"}, firstId);
            require(store.Get(file) && store.Get(file)->description == L"Parent description", "Recursive description inheritance failed");
            require(store.Get(file)->groupId == firstId && hasTag(store.Get(folder), L"Folder") &&
                hasTag(store.Get(file), L"SubFile") && hasTag(store.Get(nested), L"SubFolder") &&
                !hasTag(store.Get(file), L"Folder"), "Automatic folder and file tags were incorrect");
            store.SaveNote(file, L"Child override", {L"Own"}, secondId);
            require(store.Get(folder)->description == L"Parent description" && store.Get(file)->groupId == firstId &&
                hasTag(store.Get(file), L"SubFile"), "Child edit lost its inherited role or changed its parent");
            std::array parents{folder}; std::array children{file};
            store.SetStatus(parents, L"Done");
            require(store.Get(file)->status == L"Done" && store.Get(nested)->status == L"Done", "Status cascade failed");
            store.SetStatus(children, L"Important");
            require(store.Get(file)->status == L"Important" && store.Get(folder)->status == L"Done", "Independent status failed");
            auto statusOnly = root / L"status-only.txt";
            std::array statusPaths{statusOnly};
            store.SetStatus(statusPaths, L"Important");
            require(std::ranges::any_of(store.Search(L""), [&](auto note) { return note->path == statusOnly; }), "Blank-query search omitted status-only records");
            store.SetGroup(parents, firstId);
            require(store.Get(file)->groupId == firstId && store.Get(file)->description == L"Child override" && hasTag(store.Get(file), L"Own"), "Group-only cascade changed other metadata");
            auto future = nested / L"future.txt"; { std::ofstream output(future); output << "future"; }
            store.InheritNewChildren(nested);
            require(store.Get(future) && store.Get(future)->description == L"Parent description" && store.Get(future)->status == L"Done" &&
                hasTag(store.Get(future), L"SubFile") && !hasTag(store.Get(future), L"Folder"), "New child inheritance failed");
            std::vector<std::wstring> manyTags;
            for (int index = 0; index < 150; ++index) manyTags.push_back(L"Tag" + std::to_wstring(index));
            store.SaveNote(file, L"Many tags", manyTags, secondId);
            require(store.Get(file)->tags.size() == 151 && store.Get(file)->groupId == firstId, "Multiple tags/single group failed");
            store.SetStatus(children, L"New");
            store.SaveNote(folder, L"Updated parent", {L"Updated"}, firstId);
            require(store.Get(file)->status == L"New", "Metadata cascade changed an explicitly selected child status");
            store.SaveNote(nested, L"Independent branch", {L"Branch"}, secondId);
            require(!hasTag(store.Get(nested), L"Folder") && hasTag(store.Get(nested), L"SubFolder"), "Nested folder lost its inherited role");
            auto outside = CreateEmptyItem(folder, L"outside.txt", false);
            store.SaveNote(folder, L"Ancestor change", {L"Outside"}, firstId);
            store.SetGroup(parents, std::nullopt);
            store.SetStatus(parents, L"Important");
            require(store.Get(nested)->description == L"Independent branch" && store.Get(nested)->status == L"Important" &&
                !store.Get(nested)->groupId && store.Get(file)->description == L"Many tags" &&
                store.Get(file)->status == L"New" && !store.Get(file)->groupId &&
                hasTag(store.Get(file), L"Branch") && hasTag(store.Get(file), L"Outside"), "Nested overrides or additive parent tags failed");
            require(store.Get(outside) && store.Get(outside)->status == L"Important" && !store.Get(outside)->groupId &&
                hasTag(store.Get(outside), L"Outside"), "Ancestor stopped updating files outside the independent branch");
            auto lateFolder = CreateEmptyItem(nested, L"late.folder", true);
            store.InheritNewChildren(nested);
            auto lateFile = CreateEmptyItem(lateFolder, L"extensionless", false);
            store.InheritNewChildren(lateFolder);
            require(hasTag(store.Get(lateFolder), L"SubFolder") && store.Get(lateFile) &&
                hasTag(store.Get(lateFile), L"SubFile") && store.Get(lateFile)->description == L"Independent branch" &&
                !store.Get(lateFile)->groupId && store.Get(lateFile)->status == L"Important", "New descendants inherited across the branch boundary");
            store.SaveNote(statusOnly, L"Manual tags", { L"Folder", L"#sUbFiLe", L"SubFolder", L"SUB", L"Custom" }, std::nullopt);
            require(store.Get(statusOnly)->tags == std::vector<std::wstring>{ L"Custom" }, "Reserved tags could be assigned manually");
            auto incoming=CreateEmptyItem(root,L"existing.md",false);
            store.SaveNote(incoming,L"Handwritten",{L"Personal"},secondId);
            std::array incomingStatus{incoming}; store.SetStatus(incomingStatus,L"Emergency");
            auto entered=nested/incoming.filename();
            std::filesystem::rename(incoming,entered); store.MovePath(incoming,entered);
            require(store.Get(entered)->description==L"Handwritten" && store.Get(entered)->status==L"Emergency" &&
                !store.Get(entered)->groupId && hasTag(store.Get(entered),L"SubFile") && hasTag(store.Get(entered),L"Personal") &&
                hasTag(store.Get(entered),L"Branch"),"Moving an annotated file failed to merge metadata");
            store.SaveNote(entered,L"Handwritten",{L"Personal"},secondId);
            store.InheritNewChildren(nested);
            require(!hasTag(store.Get(entered),L"Branch"),"Refresh restored a tag the child explicitly removed");
            store.SaveNote(nested,L"Branch revision",{L"Branch",L"Later"},secondId);
            require(!hasTag(store.Get(entered),L"Branch") && hasTag(store.Get(entered),L"Later") && !store.Get(entered)->groupId,
                "An unmodified parent tag reappeared or a newly added tag was missed");
            std::array forbid{entered}; store.SetGroup(forbid,secondId);
            require(!store.Get(entered)->groupId,"Inherited group was editable");
            auto beforeParent=CreateEmptyItem(root,L"new parent",true);
            auto beforeFile=CreateEmptyItem(beforeParent,L"already noted",false);
            store.SaveNote(beforeFile,L"Older child",{L"Old"},secondId);
            store.SaveNote(beforeParent,L"Parent note",{L"Added"},firstId);
            require(store.Get(beforeFile)->description==L"Older child" && hasTag(store.Get(beforeFile),L"Added") &&
                hasTag(store.Get(beforeFile),L"Old") && store.Get(beforeFile)->groupId==firstId,"Parent overwritten pre-existing child");
            auto incomingFolder=CreateEmptyItem(root,L"incoming folder",true);
            auto unedited=CreateEmptyItem(incomingFolder,L"follows.txt",false);
            store.SaveNote(incomingFolder,L"Moved branch",{L"Mine"},secondId);
            std::array branch{incomingFolder}; store.SetStatus(branch,L"Done");
            auto movedFolder=beforeParent/incomingFolder.filename();
            std::filesystem::rename(incomingFolder,movedFolder); store.MovePath(incomingFolder,movedFolder);
            auto movedFile=movedFolder/unedited.filename();
            store.SetStatus(std::array{beforeParent},L"Emergency");
            require(hasTag(store.Get(movedFolder),L"SubFolder") && hasTag(store.Get(movedFolder),L"Added") &&
                store.Get(movedFolder)->description==L"Moved branch" && store.Get(movedFolder)->groupId==firstId &&
                store.Get(movedFile)->groupId==firstId && store.Get(movedFile)->status==L"Done" &&
                store.Get(movedFile)->description==L"Moved branch","Moved folder or nested status ownership was lost");
            auto& shortcut = store.Settings().shortcuts[9];
            shortcut.label = L"Test shortcut"; shortcut.type = L"Bash"; shortcut.target = L"printf 'quoted \\\"text\\\"'"; shortcut.icon = L"Terminal"; shortcut.iconPath = L"ShortcutIcons/slot-10.svg";
            store.Settings().shortcuts[11].target = file.wstring();
            store.Settings().appearance[0].fonts[::HudesNative::AppFont].size = 8.5; store.Settings().appearance[0].fonts[::HudesNative::TagFont].size = 24.25;
            store.Settings().detailsPinned=false; store.Settings().notesWidth=384; store.Settings().terminalHeight=321;
            store.Settings().appearance[1].fonts[AppFont]={L"Consolas",15,L"#EBDDBB"};
            store.Settings().appearance[1].colors[0]=L"#152534";
            store.Save();
            AppStore reloaded(db);
            require(!reloaded.Settings().detailsPinned && reloaded.Settings().notesWidth==384 && reloaded.Settings().terminalHeight==321 &&
                reloaded.Settings().appearance[1].fonts[AppFont].size==15 && reloaded.Settings().appearance[0].fonts[AppFont].size==8.5 &&
                reloaded.Settings().appearance[1].colors[0]==L"#152534" && reloaded.Settings().appearance[0].colors[0]!=L"#152534" &&
                reloaded.Get(entered)->ownStatus && reloaded.Get(entered)->ownMetadata,"Theme, panel size or ownership did not survive reopening");
            require(reloaded.Settings().shortcuts.size() == 12 && reloaded.Settings().shortcuts[9].target == shortcut.target && reloaded.Settings().shortcuts[9].iconPath == shortcut.iconPath && reloaded.Settings().shortcuts[11].target == file.wstring(), "Shortcut persistence failed");
            require(reloaded.Get(file)->status == L"New" && reloaded.Settings().appearance[0].fonts[::HudesNative::TagFont].size == 24.25, "Status/font persistence failed");
            auto destination = root / L"renamed";
            std::filesystem::rename(folder, destination);
            reloaded.MovePath(folder, destination);
            require(!reloaded.Get(file) && reloaded.Get(destination / L"nested" / file.filename()), "Folder move did not move descendant metadata");
            auto archiveGroup = root / L"archive" / L"First"; std::filesystem::create_directories(archiveGroup);
            reloaded.RegisterArchiveGroup(archiveGroup, firstId);
            reloaded.SaveNote(archiveGroup, L"Archive description", {L"Archive"}, secondId);
            require(reloaded.Get(archiveGroup)->archiveGroupId == firstId && reloaded.Get(archiveGroup)->groupId == secondId, "Archive identity is coupled to editable group");
            require(!reloaded.DeleteTag(L"folder") && !reloaded.DeleteTag(L"SubFolder") && !reloaded.DeleteTag(L"SubFile"), "An automatic tag could be deleted");
            bool reservedTagRejected{};
            try { reloaded.AddTag(L"#SubFile"); }
            catch (winrt::hresult_invalid_argument const&) { reservedTagRejected = true; }
            require(reservedTagRejected, "An automatic tag could be created manually");
            require(!reloaded.DeleteGroup(firstId), "Archive owner group was deleted");
            auto doomed = root / L"DeleteMe";
            auto sibling = root / L"DeleteMe-too.txt";
            std::filesystem::create_directories(doomed);
            auto doomedFile = doomed / L"Child.txt";
            { std::ofstream output(doomedFile); output << "delete"; }
            reloaded.SaveNote(doomed, L"Remove descendants", {L"Delete"}, firstId);
            reloaded.SaveNote(sibling, L"Keep sibling", {}, std::nullopt);
            reloaded.DeletePath(root / L"deleteme");
            require(!reloaded.Get(doomedFile) && reloaded.Get(sibling), "Case-insensitive descendant delete crossed a path boundary");
            reloaded.SaveNote(doomed, L"Archive descendants", {L"Zip"}, firstId);
            std::array zipSources{doomed};
            auto zipped = root / L"saved.zip";
            reloaded.ReplaceWithArchive(zipSources, zipped, firstId);
            require(!reloaded.Get(doomedFile) && reloaded.Get(zipped)->description == L"Archive descendants" &&
                !hasTag(reloaded.Get(zipped), L"Folder"), "ZIP kept descendant records or a folder-only tag");
            auto backup = root / L"backup.hudesdb"; reloaded.BackupTo(backup);
            auto binary = readBytes(backup);
            require(binary.starts_with("SALIFDB1C\n") && !binary.contains("Archive description"), "Backup was stored as readable JSON");
            require(readBytes(db).starts_with("SALIFDB1C\n"), "Database was stored as readable JSON");
            auto restoredPath = root / L"restored.json";
            AppStore restored(restoredPath); restored.RestoreFrom(backup);
            require(restored.Get(archiveGroup)->description == L"Archive description" && restored.Settings().shortcuts[9].label == L"Test shortcut", "Backup/restore failed");
            require(restored.VerifyPassword(L"1111"), "Password migration failed");
            constexpr auto livePassword = L"test live password";
            constexpr auto backupPassword = L"separate test backup password";
            restored.SetDatabasePassword(livePassword);
            require(restored.IsDatabaseProtected() && readBytes(restoredPath).starts_with("SALIFDB1P\n"), "Live database is not protected");
            auto beforeSave = readBytes(restoredPath);
            restored.Save();
            require(readBytes(restoredPath) != beforeSave, "Successive writes reused the encryption nonce");
            bool rejected{};
            try { AppStore locked(restoredPath); }
            catch (DatabasePasswordRequired const&) { rejected = true; }
            require(rejected, "Protected live database opened without a password");
            AppStore unlocked(restoredPath, livePassword);
            require(unlocked.Get(archiveGroup) && unlocked.IsDatabaseProtected(), "Live database password did not open the file");
            auto protectedBackup = root / L"protected.hudesdb";
            reloaded.BackupTo(protectedBackup, backupPassword);
            auto encrypted = readBytes(protectedBackup);
            require(encrypted.starts_with("SALIFDB1P\n") && !encrypted.contains("Archive description"), "Protected backup exposed plaintext notes");
            auto beforeRestore = readBytes(restoredPath);
            rejected = false;
            try { unlocked.RestoreFrom(protectedBackup, L"incorrect password"); }
            catch (DatabasePasswordRequired const&) { rejected = true; }
            require(rejected && readBytes(restoredPath) == beforeRestore, "Wrong backup password changed the live database");
            unlocked.RestoreFrom(protectedBackup, backupPassword);
            require(unlocked.IsDatabaseProtected() && unlocked.Get(archiveGroup), "Restore changed live protection or lost notes");
            auto alternatePath = root / L"other-install.hudesdb";
            std::filesystem::copy_file(protectedBackup, alternatePath);
            AppStore movedBackup(alternatePath, backupPassword);
            require(movedBackup.Get(archiveGroup) && movedBackup.IsDatabaseProtected(), "Copied backup cannot be opened with its password");
            unlocked.SetDatabasePassword(L"");
            require(!unlocked.IsDatabaseProtected() && readBytes(restoredPath).starts_with("SALIFDB1C\n"), "Removing the live password failed");
            auto invalid = root / L"incomplete.hudesdb";
            auto truncated = encrypted.substr(0, encrypted.size() / 2);
            { std::ofstream output(invalid, std::ios::binary); output << truncated; }
            rejected = false;
            try { unlocked.RestoreFrom(invalid, backupPassword); }
            catch (...) { rejected = true; }
            require(rejected && unlocked.Get(archiveGroup) && unlocked.Get(archiveGroup)->description == L"Archive description", "Restore accepted corruption or lost the live notes");
            rejected = false;
            try { AppStore corruptStore(invalid); }
            catch (...) { rejected = true; }
            require(rejected && readBytes(invalid) == truncated, "Opening a damaged database modified the file");
            auto legacy = root / L"legacy.json";
            { std::ofstream output(legacy); output << "{\"Version\":2,\"Notes\":[],\"Groups\":[],\"TagDefinitions\":[],\"Settings\":{}}"; }
            AppStore upgraded(legacy);
            require(readBytes(root / L"legacy.json.pre-v4.1.1.bak.json").starts_with("SALIFDB1C\n") && upgraded.Settings().shortcuts.size() == 12, "Legacy migration backup failed");
            auto prior = root / L"prior-shortcuts.json";
            {
                std::ofstream output(prior);
                output << R"({"Version":3,"Notes":[],"Groups":[],"TagDefinitions":[],"Settings":{"Shortcuts":[)";
                for (int index = 1; index < 10; ++index) output << R"({"Target":"slot-)" << index << R"("},)";
                output << R"({"Target":"slot-10"}]}})";
            }
            unlocked.RestoreFrom(prior);
            require(unlocked.Settings().shortcuts[9].target == L"slot-10", "Legacy plaintext backup could not be imported");
            AppStore priorStore(prior);
            require(priorStore.Settings().shortcuts[0].target == L"slot-1" && priorStore.Settings().shortcuts[9].target == L"slot-10" &&
                priorStore.Settings().shortcuts[10].target.empty() && priorStore.Settings().shortcuts[11].target.empty(), "Ten-slot database did not preserve assignments");
            require(readBytes(prior).starts_with("SALIFDB1C\n"), "Legacy database was not converted in place");
            auto oldWindowsDb = root / L"previous-windows.json";
            {
                std::string json = R"({"Version":3,"Notes":[],"Groups":[],"Settings":{}})";
                DATA_BLOB input{ static_cast<DWORD>(json.size()), reinterpret_cast<BYTE*>(json.data()) };
                DATA_BLOB blob{};
                winrt::check_bool(CryptProtectData(&input, L"Hudes test", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &blob));
                auto free = wil::scope_exit([&] { LocalFree(blob.pbData); });
                std::ofstream output(oldWindowsDb, std::ios::binary);
                output << "SALIF-DPAPI-1\n";
                output.write(reinterpret_cast<char const*>(blob.pbData), blob.cbData);
            }
            auto originalWindowsDb = readBytes(oldWindowsDb);
            rejected = false;
            try { AppStore pendingMigration(oldWindowsDb); }
            catch (DatabaseMigrationChoiceRequired const&) { rejected = true; }
            require(rejected && readBytes(oldWindowsDb) == originalWindowsDb, "Existing Windows-bound database changed before choosing protection");
            AppStore migrated(oldWindowsDb, {}, std::wstring_view(livePassword));
            require(migrated.IsDatabaseProtected() && readBytes(oldWindowsDb).starts_with("SALIFDB1P\n"), "Windows-bound database did not migrate to portable encryption");
            auto legacyTagsPath = root / L"legacy-tags.json";
            {
                using namespace winrt::Windows::Data::Json;
                auto legacyData = JsonObject::Parse(L"{\"Version\":3,\"Notes\":[],\"Groups\":[],\"TagDefinitions\":[\"SUB\",\"Folder\"],\"Settings\":{}}");
                auto addLegacyNote = [&](auto const& path, wchar_t const* entry)
                {
                    auto object = JsonObject::Parse(entry);
                    object.Insert(L"Path", JsonValue::CreateStringValue(path.wstring()));
                    legacyData.GetNamedArray(L"Notes").Append(object);
                };
                addLegacyNote(destination, L"{\"Tags\":[\"Folder\"],\"CascadeMetadata\":true}");
                addLegacyNote(destination / L"nested", L"{\"Tags\":[\"SUB\"],\"CascadeMetadata\":true}");
                addLegacyNote(destination / L"nested" / L"late.folder", L"{\"Tags\":[\"SUB\"]}");
                auto legacyFile = destination / L"nested" / file.filename();
                addLegacyNote(legacyFile, L"{\"Tags\":[\"SUB\",\"Folder\"]}");
                std::ofstream output(legacyTagsPath, std::ios::binary);
                output << winrt::to_string(legacyData.Stringify());
            }
            AppStore legacyTags(legacyTagsPath);
            require(std::ranges::none_of(legacyTags.Tags(), [](auto const& tag) { return tag == L"SUB"; }) &&
                hasTag(legacyTags.Get(destination), L"Folder") && hasTag(legacyTags.Get(destination / L"nested"), L"SubFolder") &&
                hasTag(legacyTags.Get(destination / L"nested" / L"late.folder"), L"SubFolder") &&
                hasTag(legacyTags.Get(destination / L"nested" / file.filename()), L"SubFile") &&
                hasTag(legacyTags.Get(destination / L"nested" / file.filename()), L"Folder (custom)"), "Legacy automatic and custom tags did not migrate");
            auto workspace = root / L"workspace";
            auto exportTarget = root / L"exports";
            std::filesystem::create_directory(workspace);
            std::filesystem::create_directory(exportTarget);
            auto firstArchive = CreateEmptyItem(workspace, L"grouped.txt", false);
            auto ungroupedArchive = CreateEmptyItem(workspace, L"ungrouped.txt", false);
            CreateEmptyItem(workspace, L"newFile[1].md", false);
            require(NextFileName(workspace, L"newFile", L".txt") == L"newFile[2].txt", "New file numbering reused an existing stem");
            AppStore workspaceStore(root / L"workspace.hudesdb");
            auto sourceGroup = workspaceStore.AddGroup(L"Original", L"#112233").id;
            auto newGroup = workspaceStore.AddGroup(L"New group", L"#223344").id;
            workspaceStore.SaveNote(firstArchive, L"گزارش & notes", { L"Test" }, sourceGroup);
            std::array archivedPaths{ firstArchive, ungroupedArchive };
            workspaceStore.SetArchived(archivedPaths, true);
            std::array regrouped{ firstArchive };
            workspaceStore.SetGroup(regrouped, newGroup);
            require(workspaceStore.IsArchived(firstArchive) && workspaceStore.Get(firstArchive)->groupId == newGroup &&
                std::filesystem::exists(firstArchive) && !std::filesystem::exists(workspace / L"archive"), "Archiving or regrouping changed a disk path");
            require(IsVirtualArchive(ArchiveRoot()) && IsVirtualArchive(ArchiveGroupPath(newGroup)) &&
                !IsVirtualArchive(firstArchive), "Virtual archive identity collided with a real file");
            auto csv = ExportWorkspace(workspace, {}, exportTarget, workspaceStore.Groups(), workspaceStore.Notes(), false, 2);
            require(csv.records == 3 && csv.parts == 2 &&
                std::filesystem::exists(csv.folder / L"Archive/New group/grouped.txt") &&
                std::filesystem::exists(csv.folder / L"Archive/unGrouped/ungrouped.txt") &&
                !std::filesystem::exists(csv.folder / L"grouped.txt") &&
                readBytes(csv.folder / L"Hudes metadata/Metadata 1.csv").contains("Archive/New group/grouped.txt"),
                "CSV export did not materialize the correct structure or split its records");
            auto copied = ExportWorkspace(workspace, {}, exportTarget, workspaceStore.Groups(), workspaceStore.Notes(), false, 2, ArchiveGroupPath(newGroup));
            require(std::filesystem::exists(copied.folder / L"grouped.txt") &&
                !std::filesystem::exists(copied.folder / L"ungrouped.txt"), "Copying a virtual group included the wrong files");
            auto xlsx = ExportWorkspace(workspace, {}, exportTarget, workspaceStore.Groups(), workspaceStore.Notes(), true, 2);
            require(xlsx.records == 3 && xlsx.parts == 2, "XLSX export missed its record limit");
            auto probe = root / L"workbook.zip";
            std::filesystem::copy_file(xlsx.folder / L"Hudes metadata/Metadata 1.xlsx", probe);
            ExtractZip(probe, root / L"workbook");
            auto sheet = readBytes(root / L"workbook/xl/worksheets/sheet1.xml");
            require(std::filesystem::exists(root / L"workbook/_rels/.rels") &&
                std::filesystem::exists(root / L"workbook/[Content_Types].xml") &&
                sheet.contains("Archive/New group/grouped.txt") && sheet.contains("&amp; notes") &&
                sheet.contains("inlineStr"), "XLSX workbook parts were incomplete");

            auto legacyDesktop = root / L"legacy desktop";
            auto legacyArchive = legacyDesktop / L"archive";
            auto physicalGroup = legacyArchive / L"Existing";
            std::filesystem::create_directories(physicalGroup);
            auto legacyArchived = CreateEmptyItem(physicalGroup, L"previous.txt", false);
            auto legacyDb = root / L"legacy-archive.json";
            {
                using namespace winrt::Windows::Data::Json;
                auto data = JsonObject::Parse(L"{\"Version\":4,\"Notes\":[],\"Groups\":[{\"Id\":\"existing\",\"Name\":\"Existing\",\"Color\":\"#334455\"}],\"Settings\":{\"DesktopFolderConfirmed\":true}}");
                data.GetNamedObject(L"Settings").Insert(L"DesktopFolder", JsonValue::CreateStringValue(legacyDesktop.wstring()));
                std::ofstream output(legacyDb, std::ios::binary);
                output << winrt::to_string(data.Stringify());
            }
            AppStore converted(legacyDb);
            require(converted.IsArchived(legacyArchived) && converted.Get(legacyArchived)->groupId == L"existing" &&
                converted.LegacyArchiveGroups().size() == 1 && std::filesystem::exists(legacyArchived), "Legacy archive migration moved or missed a file");
            std::array returnPaths{ legacyArchived };
            converted.SetArchived(returnPaths, false);
            auto returned = ExportWorkspace(legacyDesktop, converted.LegacyArchiveRoot(), exportTarget, converted.Groups(), converted.Notes(), false, 20);
            require(!converted.IsArchived(legacyArchived) && std::filesystem::exists(legacyArchived) &&
                std::filesystem::exists(returned.folder / L"previous.txt"), "Unarchived legacy file was hidden or moved");
            auto organizer = CreateEmptyItem(root, L"organizer", true);
            auto source = CreateEmptyItem(organizer, L"source", true);
            auto child = CreateEmptyItem(source, L"note.txt", false);
            auto target = CreateEmptyItem(organizer, L"target", true);
            AppStore organization(root / L"organizer.hudesdb");
            auto team = organization.AddGroup(L"Team", L"#102030").id;
            organization.AddTag(L"First");
            organization.AddTag(L"Second");
            organization.SaveNote(child, L"Keep this note", { L"First" }, team);
            organization.ReorderTag(L"Second", L"First");
            organization.RenameTag(L"First", L"Renamed");
            organization.UpdateGroup(team, L"Team renamed", L"#405060");
            std::array pins{ source, child };
            organization.SetPinned(pins, true);
            require(organization.Settings().notesEnabled && !organization.Settings().notesPinned, "Notes panel defaults changed");
            organization.SavePanelNote({ {}, L"Project", L"Shared note", { child, source } });
            auto noteId = organization.PanelNotes().front().id;
            auto moved = CopyToFolder(source, target, true);
            organization.MovePath(source, moved);
            AppStore organized(root / L"organizer.hudesdb");
            auto movedNote = organized.Get(moved / L"note.txt");
            auto editedGroup = organized.GetGroup(team);
            require(organized.Tags()[0] == L"Second" && organized.Tags()[1] == L"Renamed" &&
                hasTag(movedNote, L"Renamed") && !hasTag(movedNote, L"First"),
                "Tag order or rename did not survive restart and move");
            require(editedGroup && editedGroup->name == L"Team renamed" && editedGroup->color == L"#405060" &&
                movedNote && movedNote->groupId == team && movedNote->pinned &&
                organized.Get(moved) && organized.Get(moved)->pinned && !std::filesystem::exists(source),
                "Group edits, pinning or move lost metadata");
            auto panelNote = organized.PanelNotes().front();
            require(panelNote.id == noteId && SamePath(panelNote.files.at(0), moved / L"note.txt") &&
                SamePath(panelNote.files.at(1), moved), "Linked files did not follow folder move");
            panelNote.title = L"Renamed project";
            panelNote.resolved = true;
            organized.Settings().showHidden=true;
            organized.Settings().appearance[0].fonts[ShellFont].size=17;
            organized.Settings().terminalEnabled=false;
            organized.SavePanelNote(panelNote);
            organized.Settings().notesPinned = true;
            organized.BackupTo(root / L"linked-notes.hudesdb", L"Notebook-test-password");
            AppStore restoredNotes(root / L"restored-notes.hudesdb");
            restoredNotes.RestoreFrom(root / L"linked-notes.hudesdb", L"Notebook-test-password");
            require(restoredNotes.PanelNotes().at(0).title == panelNote.title && restoredNotes.Settings().notesPinned,
                "Portable backup lost notebook or pin preference");
            restoredNotes.DeletePath(moved);
            require(restoredNotes.PanelNotes().front().files.empty() && restoredNotes.PanelNotes().front().body == panelNote.body,
                "Deleting linked files removed note text or retained obsolete paths");
            require(restoredNotes.PanelNotes().front().resolved && restoredNotes.Settings().showHidden &&
                !restoredNotes.Settings().terminalEnabled && restoredNotes.Settings().appearance[0].fonts[ShellFont].size==17, "Backup lost resolution or panel preferences");
            restoredNotes.DeletePanelNote(noteId);
            require(AppStore(root/L"restored-notes.hudesdb").PanelNotes().empty(), "Deleted note reappeared after reopen");
            auto hiddenDir=CreateEmptyItem(root,L".private",true);
            auto hiddenChild=CreateEmptyItem(hiddenDir,L"item",false);
            auto hiddenFile=CreateEmptyItem(root,L"secret",false);
            require(SetFileAttributesW(hiddenFile.c_str(),FILE_ATTRIBUTE_HIDDEN) &&
                HiddenInFileView(hiddenFile) && HiddenInFileView(hiddenChild,root) && HiddenInFileView(root/L"~backup") &&
                !HiddenInFileView(hiddenChild) && !HiddenInFileView(root/L"regular"),"Hidden item filtering failed");
            auto testDir=CreateEmptyItem(root,L"Terminal & 100%",true);
            auto profiles=InstalledShells();
            auto cmd=std::ranges::find(profiles,L"cmd",&ShellProfile::id);
            require(cmd!=profiles.end(),"Windows Command Prompt detection failed");
            auto checkShell=[&](ShellProfile shell,std::string const& command)
            {
                auto probe=testDir/(shell.id+L".txt");
                std::mutex lock; std::string terminalOutput;
                TerminalSession terminal(shell,testDir,{90,18},[&](std::string chunk){std::lock_guard guard(lock); terminalOutput+=chunk;});
                terminal.Resize({100,20});
                terminal.Write(command+"\r");
                bool written=false;
                for(int i=0;i<220 && !written;++i)
                {
                    Sleep(50);
                    written=readBytes(probe).starts_with("hudes-ok");
                }
                if(!written) throw std::runtime_error("Interactive shell could not write in its starting folder: "+winrt::to_string(shell.id));
                std::lock_guard guard(lock);
                require(!terminalOutput.empty(),"No terminal output reached the renderer");
            };
            checkShell(*cmd,"@echo hudes-ok>cmd.txt");
            for(auto profile:profiles)
            {
                if(profile.id==L"windows-powershell")
                {
                    profile.arguments=L"-NoLogo -NoProfile";
                    checkShell(profile,"Set-Content windows-powershell.txt hudes-ok -Encoding ASCII");
                }
                if(profile.id==L"git-bash")
                {
                    profile.arguments=L"--noprofile --norc -i";
                    checkShell(profile,"echo hudes-ok > git-bash.txt");
                }
            }
            auto logo = root / L"logo.svg";
            { std::ofstream image(logo); image << "<svg xmlns='http://www.w3.org/2000/svg'/>"; }
            auto expectedLogo = readBytes(logo);
            auto importedLogo = ImportShortcutLogo(organizer, logo);
            std::filesystem::remove(logo);
            require(importedLogo.is_relative() && readBytes(organizer / importedLogo) == expectedLogo &&
                ImportShortcutLogo(organizer, importedLogo) == importedLogo, "Shortcut lost its imported logo");
            bool recursiveCopyRejected{};
            try { CopyToFolder(target, moved, false); } catch (...) { recursiveCopyRejected = true; }
            require(recursiveCopyRejected, "A folder copy into its descendant was accepted");
            std::array unpin{ moved };
            organized.SetPinned(unpin, false);
            AppStore unpinned(root / L"organizer.hudesdb");
            require(unpinned.Get(moved) && !unpinned.Get(moved)->pinned, "Unpin did not survive restart");
            report << "PASS: inheritance on moves and parent edits, independent child status, light/dark appearance persistence, panel sizing, interactive ConPTY terminal, hidden-file rules, resolved and deleted notes, linked notes, portable backup, recursive copy protection, tag editing and order, group edits, pinning, moves with metadata, virtual archive, legacy migration, workspace export, file names, automatic tags, database protection and core regressions.\n";
            return true;
        }
        catch (std::exception const& error) { report << "FAIL: " << error.what() << '\n'; }
        catch (...) { report << "FAIL: Windows operation failed\n"; }
        return false;
    }
}

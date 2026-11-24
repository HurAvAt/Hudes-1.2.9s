#include "pch.h"
#include "NativeServices.h"

#include <bcrypt.h>
#include <commdlg.h>
#include <propkey.h>
#include <propsys.h>
#include <propvarutil.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <algorithm>
#include <array>
#include <fstream>
#include <iomanip>
#include <sstream>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "propsys.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")

using namespace winrt;
using HudesNative::MetadataEntry;
using HudesNative::MetadataKind;

namespace
{
    std::filesystem::path Normalize(std::filesystem::path const& value)
    {
        try { return std::filesystem::weakly_canonical(std::filesystem::absolute(value)); }
        catch (...) { return std::filesystem::absolute(value).lexically_normal(); }
    }

    std::wstring Quote(std::wstring const& value)
    {
        std::wstring result{ L"\"" };
        size_t slashes{};
        for (auto character : value)
        {
            if (character == L'\\') { ++slashes; continue; }
            if (character == L'\"')
            {
                result.append(slashes * 2 + 1, L'\\');
                result.push_back(character);
                slashes = 0;
                continue;
            }
            result.append(slashes, L'\\');
            slashes = 0;
            result.push_back(character);
        }
        result.append(slashes * 2, L'\\');
        result.push_back(L'\"');
        return result;
    }

    std::filesystem::path PowerShellExecutable()
    {
        std::wstring folder(MAX_PATH, L'\0');
        auto length = GetSystemDirectoryW(folder.data(), static_cast<UINT>(folder.size()));
        if (!length || length >= folder.size()) throw hresult_error(E_FAIL, L"Windows ZIP support is unavailable.");
        folder.resize(length);
        auto path = std::filesystem::path(folder) / L"WindowsPowerShell" / L"v1.0" / L"powershell.exe";
        if (!std::filesystem::is_regular_file(path)) throw hresult_error(E_FAIL, L"Windows ZIP support is unavailable.");
        return path;
    }

    std::wstring PowerShellLiteral(std::wstring const& value)
    {
        std::wstring result{ L"'" };
        for (auto character : value)
        {
            result.push_back(character);
            if (character == L'\'') result.push_back(L'\'');
        }
        result.push_back(L'\'');
        return result;
    }

    std::filesystem::path TemporaryScriptPath()
    {
        std::wstring folder(MAX_PATH, L'\0');
        auto length = GetTempPathW(static_cast<DWORD>(folder.size()), folder.data());
        if (!length || length >= folder.size()) throw hresult_error(E_FAIL, L"The temporary folder is unavailable.");
        folder.resize(length);
        GUID id{};
        check_hresult(CoCreateGuid(&id));
        wchar_t name[80]{};
        swprintf_s(name, L"hudes-zip-%08lX%04hX%04hX%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX.ps1",
            id.Data1, id.Data2, id.Data3, id.Data4[0], id.Data4[1], id.Data4[2], id.Data4[3], id.Data4[4], id.Data4[5], id.Data4[6], id.Data4[7]);
        return std::filesystem::path(folder) / name;
    }

    void WritePowerShellScript(std::filesystem::path const& path, std::wstring const& text)
    {
        auto length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        if (length <= 0) throw hresult_error(E_FAIL, L"The ZIP command could not be prepared.");
        std::vector<char> bytes(static_cast<size_t>(length));
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), bytes.data(), length, nullptr, nullptr);
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) throw hresult_error(E_FAIL, L"The ZIP command could not be prepared.");
        constexpr unsigned char bom[]{ 0xEF, 0xBB, 0xBF };
        output.write(reinterpret_cast<char const*>(bom), sizeof(bom));
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!output) throw hresult_error(E_FAIL, L"The ZIP command could not be prepared.");
    }

    void RunHidden(std::wstring command, std::filesystem::path const& workingDirectory = {});

    void RunPowerShellScript(std::wstring const& script)
    {
        auto path = TemporaryScriptPath();
        auto cleanup = wil::scope_exit([&] { std::error_code ignored; std::filesystem::remove(path, ignored); });
        WritePowerShellScript(path, script);
        RunHidden(Quote(PowerShellExecutable().wstring()) + L" -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File " + Quote(path.wstring()));
    }

    void RunHidden(std::wstring command, std::filesystem::path const& workingDirectory)
    {
        STARTUPINFOW startup{ sizeof(startup) };
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION process{};
        auto buffer = std::vector<wchar_t>(command.begin(), command.end());
        buffer.push_back(L'\0');
        check_bool(CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
            workingDirectory.empty() ? nullptr : workingDirectory.c_str(), &startup, &process));
        HudesNative::unique_handle thread(process.hThread);
        HudesNative::unique_handle handle(process.hProcess);
        WaitForSingleObject(handle.get(), INFINITE);
        DWORD code{};
        check_bool(GetExitCodeProcess(handle.get(), &code));
        if (code != 0) throw hresult_error(E_FAIL, L"Windows archive support returned error " + std::to_wstring(code) + L".");
    }

    std::wstring PropertyText(REFPROPERTYKEY key, PROPVARIANT const& value)
    {
        PWSTR raw{};
        if (SUCCEEDED(PSFormatForDisplayAlloc(key, value, PDFF_DEFAULT, &raw)) && raw)
        {
            wil::unique_cotaskmem_string text(raw);
            return text.get();
        }
        if (SUCCEEDED(PropVariantToStringAlloc(value, &raw)) && raw)
        {
            wil::unique_cotaskmem_string text(raw);
            return text.get();
        }
        return {};
    }

    std::wstring CanonicalName(REFPROPERTYKEY key)
    {
        PWSTR raw{};
        if (FAILED(PSGetNameFromPropertyKey(key, &raw)) || !raw) return {};
        wil::unique_cotaskmem_string text(raw);
        return text.get();
    }

    std::wstring DisplayName(REFPROPERTYKEY key, std::wstring const& fallback)
    {
        com_ptr<IPropertyDescription> description;
        if (FAILED(PSGetPropertyDescription(key, IID_PPV_ARGS(description.put())))) return fallback;
        PWSTR raw{};
        if (FAILED(description->GetDisplayName(&raw)) || !raw) return fallback;
        wil::unique_cotaskmem_string value(raw);
        return value.get();
    }

    bool IsEditable(std::wstring const& canonical)
    {
        static constexpr std::array<std::wstring_view, 30> names
        {
            L"System.Title", L"System.Subject", L"System.Comment", L"System.Keywords", L"System.Author",
            L"System.Category", L"System.Copyright", L"System.Rating", L"System.Photo.PeopleNames",
            L"System.Photo.Event", L"System.Document.LastAuthor", L"System.Document.RevisionNumber",
            L"System.Document.Version", L"System.Document.Manager", L"System.Company", L"System.Media.Year",
            L"System.Media.Publisher", L"System.Media.Writer", L"System.Media.EncodedBy", L"System.Media.SubTitle",
            L"System.Music.AlbumTitle", L"System.Music.Artist", L"System.Music.AlbumArtist", L"System.Music.Genre",
            L"System.Music.TrackNumber", L"System.Music.Composer", L"System.Video.Director", L"System.Video.Producer",
            L"System.Video.Writer", L"System.Photo.DateTaken"
        };
        return std::ranges::find(names, canonical) != names.end();
    }

    std::wstring Category(std::wstring const& canonical)
    {
        if (canonical.starts_with(L"System.Photo.")) return L"Photo";
        if (canonical.starts_with(L"System.Image.")) return L"Image";
        if (canonical.starts_with(L"System.GPS.")) return L"Location";
        if (canonical.starts_with(L"System.Document.") || canonical == L"System.Company") return L"Document";
        if (canonical.starts_with(L"System.Music.")) return L"Music";
        if (canonical.starts_with(L"System.Audio.")) return L"Audio";
        if (canonical.starts_with(L"System.Video.")) return L"Video";
        if (canonical.starts_with(L"System.Media.")) return L"Media";
        if (canonical.starts_with(L"System.File") || canonical.starts_with(L"System.Date") || canonical.starts_with(L"System.Item") || canonical == L"System.KindText" || canonical == L"System.MIMEType") return L"File";
        return L"General";
    }

    MetadataKind Kind(std::wstring const& canonical)
    {
        if (canonical == L"System.Keywords" || canonical == L"System.Author" || canonical == L"System.Category" ||
            canonical == L"System.Photo.PeopleNames" || canonical == L"System.Photo.Event" ||
            canonical == L"System.Media.Writer" || canonical == L"System.Music.Artist" ||
            canonical == L"System.Music.Genre" || canonical == L"System.Music.Composer" ||
            canonical == L"System.Video.Director" || canonical == L"System.Video.Producer" || canonical == L"System.Video.Writer")
            return MetadataKind::TextList;
        if (canonical == L"System.Rating" || canonical == L"System.Media.Year" || canonical == L"System.Music.TrackNumber")
            return MetadataKind::UnsignedInteger;
        return MetadataKind::Text;
    }

    void AppendKnownProperties(IPropertyStore* store, std::vector<HudesNative::MetadataEntry>& result)
    {
        static constexpr std::array<std::wstring_view, 75> names
        {
            L"System.Title", L"System.Subject", L"System.Comment", L"System.Keywords", L"System.Author", L"System.Category", L"System.Copyright", L"System.Rating",
            L"System.FileOwner", L"System.KindText", L"System.ItemTypeText", L"System.MIMEType", L"System.DateCreated", L"System.DateModified", L"System.DateAccessed", L"System.FileAttributesDisplay",
            L"System.Image.Dimensions", L"System.Image.HorizontalSize", L"System.Image.VerticalSize", L"System.Image.BitDepth", L"System.Image.Compression", L"System.Image.ColorSpace", L"System.Image.HorizontalResolution", L"System.Image.VerticalResolution",
            L"System.Photo.CameraManufacturer", L"System.Photo.CameraModel", L"System.Photo.DateTaken", L"System.Photo.PeopleNames", L"System.Photo.Event", L"System.Photo.ExposureTime", L"System.Photo.FNumber", L"System.Photo.ExposureProgramText", L"System.Photo.ExposureBiasText", L"System.Photo.ISOSpeed", L"System.Photo.FocalLength", L"System.Photo.FlashText", L"System.Photo.OrientationText", L"System.Photo.WhiteBalanceText", L"System.Photo.MeteringModeText", L"System.Photo.LensManufacturer", L"System.Photo.LensModel", L"System.Photo.DigitalZoom",
            L"System.GPS.Latitude", L"System.GPS.LatitudeRef", L"System.GPS.Longitude", L"System.GPS.LongitudeRef", L"System.GPS.Altitude", L"System.GPS.AltitudeRef", L"System.GPS.Date", L"System.GPS.Speed", L"System.GPS.DestinationBearing",
            L"System.Document.LastAuthor", L"System.Document.RevisionNumber", L"System.Document.Version", L"System.Document.Template", L"System.Document.Manager", L"System.Company", L"System.Document.DateCreated", L"System.Document.DateSaved", L"System.Document.DatePrinted", L"System.Document.PageCount", L"System.Document.WordCount", L"System.Document.CharacterCount", L"System.Document.LineCount", L"System.Document.ParagraphCount", L"System.Document.TotalEditingTime",
            L"System.Media.Duration", L"System.Media.Year", L"System.Media.Publisher", L"System.Media.Writer", L"System.Media.EncodedBy", L"System.Media.DateEncoded", L"System.Media.SubTitle",
            L"System.Music.AlbumTitle", L"System.Music.Artist"
        };
        for (auto const& name : names)
        {
            PROPERTYKEY key{};
            if (FAILED(PSGetPropertyKeyFromName(name.data(), &key))) continue;
            PROPVARIANT raw{};
            PropVariantInit(&raw);
            auto clear = wil::scope_exit([&] { PropVariantClear(&raw); });
            if (FAILED(store->GetValue(key, &raw))) continue;
            auto value = PropertyText(key, raw);
            auto canonical = std::wstring{ name };
            auto found = std::ranges::find_if(result, [&](auto const& entry) { return entry.canonicalName == canonical; });
            if (found != result.end()) continue;
            if (value.empty() && !IsEditable(canonical)) continue;
            result.push_back({ Category(canonical), DisplayName(key, canonical), canonical, value, value, IsEditable(canonical), Kind(canonical) });
        }
    }

    std::filesystem::path ShellResult(IShellItem* item)
    {
        PWSTR raw{};
        check_hresult(item->GetDisplayName(SIGDN_FILESYSPATH, &raw));
        wil::unique_cotaskmem_string value(raw);
        return value.get();
    }

    std::optional<std::filesystem::path> ShowFileDialog(HWND owner, bool save, bool folder,
        std::wstring const& title, std::filesystem::path const& initial, std::wstring const& filterName,
        std::wstring const& extension, std::wstring const& suggestedName)
    {
        com_ptr<IFileDialog> dialog;
        check_hresult(CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.put())));
        dialog->SetTitle(title.c_str());
        DWORD options{};
        check_hresult(dialog->GetOptions(&options));
        options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
        if (folder) options |= FOS_PICKFOLDERS;
        if (save) options |= FOS_OVERWRITEPROMPT;
        else if (!folder) options |= FOS_FILEMUSTEXIST;
        check_hresult(dialog->SetOptions(options));
        if (!initial.empty())
        {
            com_ptr<IShellItem> location;
            if (SUCCEEDED(SHCreateItemFromParsingName(initial.c_str(), nullptr, IID_PPV_ARGS(location.put())))) dialog->SetFolder(location.get());
        }
        COMDLG_FILTERSPEC spec{ filterName.c_str(), extension.c_str() };
        if (!filterName.empty()) dialog->SetFileTypes(1, &spec);
        if (!suggestedName.empty()) dialog->SetFileName(suggestedName.c_str());
        auto const result = dialog->Show(owner);
        if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return std::nullopt;
        check_hresult(result);
        com_ptr<IShellItem> item;
        check_hresult(dialog->GetResult(item.put()));
        return ShellResult(item.get());
    }
}

namespace HudesNative
{
    DirectoryWatcher::~DirectoryWatcher() { Stop(); }

    void DirectoryWatcher::Start(std::filesystem::path const& folder, std::function<void()> callback)
    {
        Stop();
        m_stopEvent.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        check_bool(static_cast<bool>(m_stopEvent));
        auto stop = m_stopEvent.get();
        auto path = folder.wstring();
        m_thread = std::jthread([path = std::move(path), stop, callback = std::move(callback)](std::stop_token token)
        {
            auto change = FindFirstChangeNotificationW(path.c_str(), FALSE,
                FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE);
            if (change == INVALID_HANDLE_VALUE) return;
            auto close = wil::scope_exit([&] { FindCloseChangeNotification(change); });
            HANDLE handles[]{ stop, change };
            while (!token.stop_requested())
            {
                auto const result = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
                if (result == WAIT_OBJECT_0 || result == WAIT_FAILED) break;
                if (result == WAIT_OBJECT_0 + 1)
                {
                    try { callback(); } catch (...) {}
                    if (!FindNextChangeNotification(change)) break;
                }
            }
        });
    }

    void DirectoryWatcher::Stop() noexcept
    {
        if (m_thread.joinable())
        {
            m_thread.request_stop();
            if (m_stopEvent) SetEvent(m_stopEvent.get());
            m_thread.join();
        }
        m_stopEvent.reset();
    }

    std::filesystem::path ExecutableFolder()
    {
        std::wstring buffer(MAX_PATH, L'\0');
        for (;;)
        {
            auto const length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (!length) throw_last_error();
            if (length < buffer.size() - 1) { buffer.resize(length); return std::filesystem::path(buffer).parent_path(); }
            buffer.resize(buffer.size() * 2);
        }
    }

    bool SamePath(std::filesystem::path const& left, std::filesystem::path const& right) noexcept
    {
        try { return _wcsicmp(Normalize(left).c_str(), Normalize(right).c_str()) == 0; }
        catch (...) { return false; }
    }

    bool IsWithin(std::filesystem::path const& path, std::filesystem::path const& folder) noexcept
    {
        try
        {
            auto value = Normalize(path).wstring();
            auto parent = Normalize(folder).wstring();
            if (!parent.ends_with(L'\\')) parent.push_back(L'\\');
            return value.size() >= parent.size() && _wcsnicmp(value.c_str(), parent.c_str(), parent.size()) == 0;
        }
        catch (...) { return false; }
    }

    std::filesystem::path UniqueDestination(std::filesystem::path const& path)
    {
        if (!std::filesystem::exists(path)) return path;
        auto directory = path.parent_path();
        auto name = path.stem().wstring();
        auto extension = path.extension().wstring();
        for (int index = 2;; ++index)
        {
            auto candidate = directory / (name + L" (" + std::to_wstring(index) + L")" + extension);
            if (!std::filesystem::exists(candidate)) return candidate;
        }
    }

    void ValidateItemName(std::wstring_view name)
    {
        if (name.empty() || name.back() == L'.' || name.back() == L' ' ||
            name.find_first_of(L"<>:\"/\\|?*") != name.npos ||
            std::ranges::any_of(name, [](wchar_t character) { return character < 32; }))
            throw hresult_invalid_argument(L"Enter a valid Windows name without path separators, trailing spaces, or trailing dots.");

        auto device = std::wstring(name.substr(0, name.find(L'.')));
        while (!device.empty() && device.back() == L' ') device.pop_back();
        for (auto const* reserved : { L"CON", L"PRN", L"AUX", L"NUL", L"CONIN$", L"CONOUT$" })
            if (_wcsicmp(device.c_str(), reserved) == 0)
                throw hresult_invalid_argument(L"That name is reserved by Windows.");

        if (device.size() == 4 && wcschr(L"123456789¹²³", device.back()) &&
            (_wcsnicmp(device.c_str(), L"COM", 3) == 0 || _wcsnicmp(device.c_str(), L"LPT", 3) == 0))
            throw hresult_invalid_argument(L"That name is reserved by Windows.");
    }

    std::filesystem::path CreateEmptyItem(std::filesystem::path const& folder, std::wstring const& name, bool directory)
    {
        ValidateItemName(name);
        auto path = folder / name;
        if (directory)
        {
            if (CreateDirectoryW(path.c_str(), nullptr)) return path;
        }
        else
        {
            auto file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE)
            {
                CloseHandle(file);
                return path;
            }
        }
        auto error = GetLastError();
        if (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS)
            throw hresult_error(HRESULT_FROM_WIN32(error), L"An item with that name already exists.");
        throw_hresult(HRESULT_FROM_WIN32(error));
    }

    std::wstring FriendlyError(std::exception_ptr error)
    {
        try { if (error) std::rethrow_exception(error); }
        catch (hresult_error const& value) { return value.message().c_str(); }
        catch (std::filesystem::filesystem_error const& value) { return to_hstring(value.code().message()).c_str(); }
        catch (std::exception const& value) { return to_hstring(value.what()).c_str(); }
        catch (...) { return L"The operation could not be completed."; }
        return {};
    }

    std::wstring FormatSize(std::uintmax_t size)
    {
        wchar_t buffer[64]{};
        if (size >= 1073741824ull) swprintf_s(buffer, L"%.2f GB", size / 1073741824.0);
        else if (size >= 1048576ull) swprintf_s(buffer, L"%.2f MB", size / 1048576.0);
        else if (size >= 1024ull) swprintf_s(buffer, L"%.2f KB", size / 1024.0);
        else swprintf_s(buffer, L"%llu B", static_cast<unsigned long long>(size));
        return buffer;
    }

    std::wstring FileTimeText(std::filesystem::file_time_type value)
    {
        try
        {
            auto system = std::chrono::clock_cast<std::chrono::system_clock>(value);
            auto time = std::chrono::system_clock::to_time_t(system);
            tm local{};
            localtime_s(&local, &time);
            wchar_t buffer[32]{};
            wcsftime(buffer, std::size(buffer), L"%Y-%m-%d  %H:%M", &local);
            return buffer;
        }
        catch (...) { return {}; }
    }

    bool FileNameMatches(std::wstring const& fileName, std::wstring const& query)
    {
        std::wistringstream input(query);
        for (std::wstring term; input >> term;)
        {
            if (term.find_first_of(L"*?") != std::wstring::npos)
            {
                if (PathMatchSpecExW(fileName.c_str(), term.c_str(), PMSF_NORMAL) != S_OK) return false;
            }
            else if (StrStrIW(fileName.c_str(), term.c_str()) == nullptr) return false;
        }
        return true;
    }

    bool ReminderIsNear(std::optional<std::wstring> const& value, double warningHours)
    {
        if (!value) return false;
        int year{}, month{}, day{}, hour{}, minute{}, second{};
        if (swscanf_s(value->c_str(), L"%d-%d-%dT%d:%d:%d", &year, &month, &day, &hour, &minute, &second) < 5) return false;
        tm date{};
        date.tm_year = year - 1900; date.tm_mon = month - 1; date.tm_mday = day;
        date.tm_hour = hour; date.tm_min = minute; date.tm_sec = second; date.tm_isdst = -1;
        auto const target = std::chrono::system_clock::from_time_t(mktime(&date));
        return target <= std::chrono::system_clock::now() + std::chrono::duration<double, std::ratio<3600>>(warningHours);
    }

    std::wstring LocalDateTimeText(SYSTEMTIME const& value)
    {
        wchar_t buffer[40]{};
        swprintf_s(buffer, L"%04hu-%02hu-%02huT%02hu:%02hu:%02hu", value.wYear, value.wMonth, value.wDay,
            value.wHour, value.wMinute, value.wSecond);
        return buffer;
    }

    std::optional<std::filesystem::path> PickFolder(HWND owner, std::wstring const& title, std::filesystem::path const& initial)
    {
        return ShowFileDialog(owner, false, true, title, initial, {}, {}, {});
    }

    std::optional<std::filesystem::path> PickOpenFile(HWND owner, std::wstring const& title, std::wstring const& filterName, std::wstring const& extension)
    {
        return ShowFileDialog(owner, false, false, title, {}, filterName, extension, {});
    }

    std::optional<std::filesystem::path> PickSaveFile(HWND owner, std::wstring const& title, std::wstring const& filterName,
        std::wstring const& extension, std::wstring const& suggestedName)
    {
        return ShowFileDialog(owner, true, false, title, {}, filterName, extension, suggestedName);
    }

    void OpenPath(std::filesystem::path const& path)
    {
        auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, path.parent_path().c_str(), SW_SHOWNORMAL));
        if (result <= 32) throw hresult_error(E_FAIL, L"Windows could not open the selected item.");
    }

    void ShowOpenWith(HWND owner, std::filesystem::path const& path)
    {
        if (!std::filesystem::is_regular_file(path)) throw hresult_invalid_argument(L"Open with is available for files only.");
        OPENASINFO information{ path.c_str(), nullptr, OAIF_EXEC };
        check_hresult(SHOpenWithDialog(owner, &information));
    }

    void ShowInExplorer(std::filesystem::path const& path)
    {
        auto arguments = L"/select," + Quote(path.wstring());
        auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", L"explorer.exe", arguments.c_str(), nullptr, SW_SHOWNORMAL));
        if (result <= 32) throw hresult_error(E_FAIL, L"Windows could not show the item in File Explorer.");
    }

    std::filesystem::path CopyToFolder(std::filesystem::path const& source, std::filesystem::path const& folder, bool move)
    {
        if (std::filesystem::is_directory(source) && (SamePath(source, folder) || IsWithin(folder, source)))
            throw hresult_invalid_argument(L"Choose a destination outside the selected folder.");
        auto destination = UniqueDestination(folder / source.filename());
        if (move)
        {
            MoveTo(source, destination);
            return destination;
        }
        if (std::filesystem::is_directory(source))
            std::filesystem::copy(source, destination, std::filesystem::copy_options::recursive);
        else
            std::filesystem::copy_file(source, destination);
        return destination;
    }

    void MoveTo(std::filesystem::path const& source, std::filesystem::path const& destination)
    {
        std::filesystem::create_directories(destination.parent_path());
        if (MoveFileExW(source.c_str(), destination.c_str(), 0)) return;
        auto error = GetLastError();
        if (error != ERROR_NOT_SAME_DEVICE) throw_hresult(HRESULT_FROM_WIN32(error));
        if (std::filesystem::is_directory(source))
        {
            if (!std::filesystem::create_directory(destination))
                throw hresult_error(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), L"The destination already exists.");
            std::filesystem::copy(source, destination, std::filesystem::copy_options::recursive);
            std::filesystem::remove_all(source);
        }
        else
        {
            std::filesystem::copy_file(source, destination);
            std::filesystem::remove(source);
        }
    }

    void DeleteToRecycleBin(HWND owner, std::span<std::filesystem::path const> paths)
    {
        com_ptr<IFileOperation> operation;
        check_hresult(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(operation.put())));
        check_hresult(operation->SetOwnerWindow(owner));
        check_hresult(operation->SetOperationFlags(FOF_ALLOWUNDO | FOF_NOCONFIRMMKDIR | FOFX_ADDUNDORECORD));
        for (auto const& path : paths)
        {
            com_ptr<IShellItem> item;
            check_hresult(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(item.put())));
            check_hresult(operation->DeleteItem(item.get(), nullptr));
        }
        check_hresult(operation->PerformOperations());
        BOOL aborted{};
        check_hresult(operation->GetAnyOperationsAborted(&aborted));
        if (aborted) throw hresult_error(HRESULT_FROM_WIN32(ERROR_CANCELLED));
    }

    void SetClipboardFiles(HWND owner, std::span<std::filesystem::path const> paths, bool cut)
    {
        if (paths.empty()) return;
        size_t characters = 1;
        for (auto const& path : paths) characters += path.native().size() + 1;
        auto memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DROPFILES) + characters * sizeof(wchar_t));
        if (!memory) throw std::bad_alloc();
        auto release = wil::scope_exit([&] { if (memory) GlobalFree(memory); });
        auto drop = static_cast<DROPFILES*>(GlobalLock(memory));
        check_pointer(drop);
        drop->pFiles = sizeof(DROPFILES);
        drop->fWide = TRUE;
        auto output = reinterpret_cast<wchar_t*>(reinterpret_cast<std::byte*>(drop) + sizeof(DROPFILES));
        for (auto const& path : paths)
        {
            auto const& value = path.native();
            std::copy(value.begin(), value.end(), output);
            output += value.size();
            *output++ = L'\0';
        }
        *output = L'\0';
        GlobalUnlock(memory);
        check_bool(OpenClipboard(owner));
        auto close = wil::scope_exit([] { CloseClipboard(); });
        check_bool(EmptyClipboard());
        check_bool(SetClipboardData(CF_HDROP, memory));
        memory = nullptr;
        auto format = RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
        auto effectMemory = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
        if (effectMemory)
        {
            auto effect = static_cast<DWORD*>(GlobalLock(effectMemory));
            if (effect)
            {
                *effect = cut ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
                GlobalUnlock(effectMemory);
                if (!SetClipboardData(format, effectMemory)) GlobalFree(effectMemory);
            }
            else GlobalFree(effectMemory);
        }
    }

    std::vector<std::filesystem::path> GetClipboardFiles(bool& move)
    {
        move = false;
        std::vector<std::filesystem::path> paths;
        check_bool(OpenClipboard(nullptr));
        auto close = wil::scope_exit([] { CloseClipboard(); });
        auto drop = static_cast<HDROP>(GetClipboardData(CF_HDROP));
        if (!drop) return paths;
        auto const count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT index = 0; index < count; ++index)
        {
            auto const length = DragQueryFileW(drop, index, nullptr, 0);
            std::wstring value(length + 1, L'\0');
            DragQueryFileW(drop, index, value.data(), static_cast<UINT>(value.size()));
            value.resize(length);
            paths.emplace_back(std::move(value));
        }
        auto format = RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
        if (auto memory = GetClipboardData(format))
        {
            if (auto effect = static_cast<DWORD*>(GlobalLock(memory))) { move = (*effect & DROPEFFECT_MOVE) != 0; GlobalUnlock(memory); }
        }
        return paths;
    }

    std::filesystem::path ImportShortcutLogo(std::filesystem::path const& app, std::filesystem::path source)
    {
        if (source.is_relative()) source = app / source;
        if (!std::filesystem::is_regular_file(source)) source = app / L"logo" / source.filename();
        if (!std::filesystem::is_regular_file(source)) throw hresult_invalid_argument(L"Choose an existing logo file.");
        auto directory = app / L"ShortcutIcons";
        std::filesystem::create_directories(directory);
        if (SamePath(source.parent_path(), directory)) return std::filesystem::path(L"ShortcutIcons") / source.filename();
        GUID id{};
        check_hresult(CoCreateGuid(&id));
        wchar_t unique[40]{};
        StringFromGUID2(id, unique, 40);
        auto staging = directory / (std::wstring(unique) + L".tmp");
        auto cleanup = wil::scope_exit([&] { std::error_code ignored; std::filesystem::remove(staging, ignored); });
        std::filesystem::copy_file(source, staging);
        auto relative = std::filesystem::path(L"ShortcutIcons") /
            (CalculateFileHash(staging, FileHashAlgorithm::Sha256) + source.extension().wstring());
        check_bool(MoveFileExW(staging.c_str(), (app / relative).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH));
        return relative;
    }

    std::wstring CalculateFileHash(std::filesystem::path const& path, FileHashAlgorithm algorithm)
    {
        auto algorithmName = algorithm == FileHashAlgorithm::Sha256 ? BCRYPT_SHA256_ALGORITHM : BCRYPT_MD5_ALGORITHM;
        BCRYPT_ALG_HANDLE rawAlgorithm{};
        check_nt(BCryptOpenAlgorithmProvider(&rawAlgorithm, algorithmName, nullptr, 0));
        auto closeAlgorithm = wil::scope_exit([&] { BCryptCloseAlgorithmProvider(rawAlgorithm, 0); });
        ULONG digestLength{}, bytesWritten{};
        check_nt(BCryptGetProperty(rawAlgorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&digestLength),
            sizeof(digestLength), &bytesWritten, 0));
        BCRYPT_HASH_HANDLE rawHash{};
        check_nt(BCryptCreateHash(rawAlgorithm, &rawHash, nullptr, 0, nullptr, 0, 0));
        auto hash = wil::scope_exit([&] { BCryptDestroyHash(rawHash); });
        std::ifstream input(path, std::ios::binary);
        if (!input) throw hresult_error(E_FAIL, L"The selected file could not be read.");
        std::vector<std::uint8_t> buffer(1024 * 1024);
        while (input)
        {
            input.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
            auto const read = input.gcount();
            if (read > 0) check_nt(BCryptHashData(rawHash, buffer.data(), static_cast<ULONG>(read), 0));
        }
        if (input.bad() || !input.eof()) throw hresult_error(E_FAIL, L"The selected file could not be read completely.");
        std::vector<std::uint8_t> digest(digestLength);
        check_nt(BCryptFinishHash(rawHash, digest.data(), digestLength, 0));
        std::wostringstream output;
        output << std::hex << std::setfill(L'0');
        for (auto value : digest) output << std::setw(2) << static_cast<unsigned>(value);
        return output.str();
    }

    std::vector<std::filesystem::path> CreateZip(std::span<std::filesystem::path const> sources,
        std::filesystem::path const& outputZip)
    {
        if (sources.empty()) return {};
        auto temporary = outputZip;
        temporary += L".hudes-building.zip";
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        auto cleanup = wil::scope_exit([&] { std::error_code error; std::filesystem::remove(temporary, error); });
        std::wstring script = L"$ErrorActionPreference = 'Stop'\r\nCompress-Archive -LiteralPath @(\r\n";
        for (size_t index = 0; index < sources.size(); ++index)
            script += L"  " + PowerShellLiteral(sources[index].wstring()) + (index + 1 == sources.size() ? L"\r\n" : L",\r\n");
        script += L") -DestinationPath " + PowerShellLiteral(temporary.wstring()) + L" -CompressionLevel Optimal -Force\r\n";
        script += L"if (-not (Test-Path -LiteralPath " + PowerShellLiteral(temporary.wstring()) + L")) { throw 'The ZIP file was not created.' }\r\n";
        RunPowerShellScript(script);
        check_bool(MoveFileExW(temporary.c_str(), outputZip.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED));
        return { outputZip };
    }

    void ExtractZip(std::filesystem::path const& archivePath, std::filesystem::path const& destination)
    {
        if (_wcsicmp(archivePath.extension().c_str(), L".zip") != 0)
            throw hresult_invalid_argument(L"Select a standard .zip file.");
        std::filesystem::create_directories(destination);
        auto script = L"$ErrorActionPreference = 'Stop'\r\nExpand-Archive -LiteralPath " + PowerShellLiteral(archivePath.wstring()) +
            L" -DestinationPath " + PowerShellLiteral(destination.wstring()) + L" -Force\r\n";
        RunPowerShellScript(script);
    }

    std::vector<MetadataEntry> ReadMetadata(std::filesystem::path const& path)
    {
        std::vector<MetadataEntry> result;
        auto information = std::filesystem::status(path);
        result.push_back({ L"File", L"Type", {}, std::filesystem::is_directory(information) ? L"Folder" : path.extension().wstring(), {}, false });
        result.push_back({ L"File", L"Location", {}, path.parent_path().wstring(), {}, false });
        if (std::filesystem::is_regular_file(information))
            result.push_back({ L"File", L"Size", {}, FormatSize(std::filesystem::file_size(path)), {}, false });
        try { result.push_back({ L"File", L"Modified", {}, FileTimeText(std::filesystem::last_write_time(path)), {}, false }); } catch (...) {}
        if (std::filesystem::is_directory(information)) return result;

        com_ptr<IShellItem2> item;
        check_hresult(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(item.put())));
        com_ptr<IPropertyStore> store;
        check_hresult(item->GetPropertyStore(GPS_BESTEFFORT, IID_PPV_ARGS(store.put())));
        DWORD count{};
        check_hresult(store->GetCount(&count));
        for (DWORD index = 0; index < count; ++index)
        {
            PROPERTYKEY key{};
            if (FAILED(store->GetAt(index, &key))) continue;
            auto canonical = CanonicalName(key);
            if (canonical.empty()) continue;
            PROPVARIANT raw{};
            PropVariantInit(&raw);
            auto clear = wil::scope_exit([&] { PropVariantClear(&raw); });
            if (FAILED(store->GetValue(key, &raw))) continue;
            auto value = PropertyText(key, raw);
            if (value.empty() && !IsEditable(canonical)) continue;
            result.push_back({ Category(canonical), DisplayName(key, canonical), canonical, value, value, IsEditable(canonical), Kind(canonical) });
        }
        AppendKnownProperties(store.get(), result);
        static constexpr std::array<std::wstring_view, 11> order{ L"General", L"File", L"Image", L"Photo", L"Location", L"Document", L"Media", L"Music", L"Audio", L"Video", L"Additional" };
        std::ranges::sort(result, [&](auto const& left, auto const& right)
        {
            auto leftRank = std::ranges::find(order, left.category) - order.begin();
            auto rightRank = std::ranges::find(order, right.category) - order.begin();
            if (leftRank != rightRank) return leftRank < rightRank;
            return _wcsicmp(left.name.c_str(), right.name.c_str()) < 0;
        });
        return result;
    }

    std::vector<std::wstring> SaveMetadata(std::filesystem::path const& path, std::span<MetadataEntry> entries)
    {
        std::vector<std::wstring> failures;
        com_ptr<IShellItem2> item;
        check_hresult(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(item.put())));
        com_ptr<IPropertyStore> store;
        check_hresult(item->GetPropertyStore(GPS_READWRITE, IID_PPV_ARGS(store.put())));
        for (auto& entry : entries)
        {
            if (!entry.editable || entry.value == entry.originalValue || entry.canonicalName.empty()) continue;
            try
            {
                PROPERTYKEY key{};
                check_hresult(PSGetPropertyKeyFromName(entry.canonicalName.c_str(), &key));
                PROPVARIANT value{};
                PropVariantInit(&value);
                auto clear = wil::scope_exit([&] { PropVariantClear(&value); });
                if (entry.kind == MetadataKind::UnsignedInteger)
                {
                    wchar_t* end{};
                    auto number = wcstoul(entry.value.c_str(), &end, 10);
                    if (end == entry.value.c_str() || *end != L'\0') throw hresult_invalid_argument(L"Enter a non-negative whole number.");
                    check_hresult(InitPropVariantFromUInt32(number, &value));
                }
                else if (entry.kind == MetadataKind::TextList)
                {
                    std::vector<std::wstring> parts;
                    std::wstring current;
                    for (auto character : entry.value)
                    {
                        if (character == L';' || character == L',' || character == L'،' || character == L'\n')
                        {
                            if (!current.empty()) { parts.push_back(current); current.clear(); }
                        }
                        else current.push_back(character);
                    }
                    if (!current.empty()) parts.push_back(current);
                    std::vector<PCWSTR> pointers;
                    for (auto const& part : parts) pointers.push_back(part.c_str());
                    check_hresult(InitPropVariantFromStringVector(pointers.data(), static_cast<ULONG>(pointers.size()), &value));
                }
                else check_hresult(InitPropVariantFromString(entry.value.c_str(), &value));
                com_ptr<IPropertyDescription> description;
                if (SUCCEEDED(PSGetPropertyDescription(key, IID_PPV_ARGS(description.put())))) description->CoerceToCanonicalValue(&value);
                check_hresult(store->SetValue(key, value));
                entry.originalValue = entry.value;
            }
            catch (...) { failures.push_back(entry.name + L": " + FriendlyError()); }
        }
        if (failures.empty()) check_hresult(store->Commit());
        else if (SUCCEEDED(store->Commit())) {}
        return failures;
    }
}

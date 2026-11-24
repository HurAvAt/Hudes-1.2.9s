#include "pch.h"
#include "CoreTests.h"
#include "AppStore.h"
#include "NativeServices.h"
#include "Terminal.h"
#include <algorithm>
#include <array>
#include <fstream>
#include <psapi.h>
#ifdef _DEBUG
#include <crtdbg.h>
#endif

namespace fs = std::filesystem;

namespace
{
    struct Usage
    {
        DWORD handles{}, gdi{}, user{};
        size_t privateBytes{};
    };

    Usage Sample()
    {
        Usage usage;
        auto process = GetCurrentProcess();
        winrt::check_bool(GetProcessHandleCount(process, &usage.handles));
        usage.gdi = GetGuiResources(process, GR_GDIOBJECTS);
        usage.user = GetGuiResources(process, GR_USEROBJECTS);
        PROCESS_MEMORY_COUNTERS_EX memory{};
        winrt::check_bool(K32GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)));
        usage.privateBytes = memory.PrivateUsage;
        return usage;
    }
}

namespace HudesNative
{
    bool RunResourceTests()
    {
        std::ofstream report(ExecutableFolder()/L"resource-test-results.txt");
        try
        {
            GUID id{};
            winrt::check_hresult(CoCreateGuid(&id));
            wchar_t token[40]{};
            StringFromGUID2(id, token, ARRAYSIZE(token));
            auto root = fs::temp_directory_path() / (L"hudes-lifetime-" + std::wstring(token));
            fs::create_directory(root);
            auto cleanup = wil::scope_exit([&] { std::error_code error; fs::remove_all(root, error); });
            auto file = root/L"file.txt";
            std::ofstream(file) << "Hudes";
            auto shells = InstalledShells();
            auto cmd = std::ranges::find(shells,L"cmd",&ShellProfile::id);
            if (cmd == shells.end()) throw std::runtime_error("CMD unavailable");

            auto cycle = [&](int number)
            {
                DirectoryWatcher watcher;
                for (int i=0; i<3; ++i) watcher.Start(root,[]{});
                AppStore store(root/L"test.db");
                auto folder=root/L"managed";
                fs::create_directories(folder);
                auto child=folder/L"note.txt"; std::ofstream(child) << "child";
                store.SaveNote(folder,L"Parent",{L"One"},{});
                store.SaveNote(child,L"Independent",{L"Local"},{});
                store.SaveNote(folder,L"Updated",{L"One",L"Two"},{});
                store.InheritNewChildren(folder);
                store.SavePanelNote({{},L"Lifetime test",L"Example",{file}});
                auto note = store.PanelNotes().front().id;
                store.DeletePanelNote(note);
                store.BackupTo(root/L"copy.db");
                store.RestoreFrom(root/L"copy.db");
                CalculateFileHash(file,FileHashAlgorithm::Sha256);
                watcher.Stop();
                wil::unique_event eof;
                eof.create();
                TerminalSession terminal(*cmd,root,{80,20},[&](std::string part){if(part.empty()) eof.SetEvent();});
                terminal.Resize({90,24});
                if (number%2 == 0)
                {
                    terminal.Write("exit\r");
                    if (WaitForSingleObject(eof.get(),8000) != WAIT_OBJECT_0)
                        throw std::runtime_error("The shell exited without releasing its pseudoconsole");
                }
            };

            if (!ShortcutFieldsReleased()) throw std::runtime_error("Shortcut controls survived editor dismissal");
            for (int i=0; i<6; ++i) cycle(i);
            for (int i=0; i<100; ++i)
                if (!ShortcutFieldsReleased()) throw std::runtime_error("A shortcut field retained a control");
            auto before = Sample();
#ifdef _DEBUG
            _CrtMemState heapBefore{}, heapAfter{};
            _CrtMemCheckpoint(&heapBefore);
#endif
            for (int i=0; i<40; ++i) cycle(i);
#ifdef _DEBUG
            _CrtMemCheckpoint(&heapAfter);
            auto blockDelta = static_cast<intptr_t>(heapAfter.lCounts[_NORMAL_BLOCK]) - static_cast<intptr_t>(heapBefore.lCounts[_NORMAL_BLOCK]);
            auto byteDelta = static_cast<intptr_t>(heapAfter.lSizes[_NORMAL_BLOCK]) - static_cast<intptr_t>(heapBefore.lSizes[_NORMAL_BLOCK]);
            report << "Debug CRT live allocation change: " << blockDelta << " blocks, " << byteDelta << " bytes.\n";
            if (blockDelta > 0 || byteDelta > 0) throw std::runtime_error("Live C++ heap allocations grew");
#endif
            auto after = Sample();
            report << "Before / after 40 cycles: handles " << before.handles << " / " << after.handles
                   << ", GDI " << before.gdi << " / " << after.gdi << ", USER " << before.user << " / " << after.user
                   << ", private bytes " << before.privateBytes << " / " << after.privateBytes << ".\n";
            if (after.handles > before.handles+2 || after.gdi > before.gdi || after.user > before.user)
                throw std::runtime_error("Windows resource counts grew");
            report << "PASS: shortcut controls released, repeated file watchers, database work, shell exit and shell shutdown.\n";
            return true;
        }
        catch (winrt::hresult_error const& e) { report << "FAIL: " << winrt::to_string(e.message()) << '\n'; }
        catch (std::exception const& e) { report << "FAIL: " << e.what() << '\n'; }
        catch (...) { report << "FAIL: Windows operation failed.\n"; }
        return false;
    }
}

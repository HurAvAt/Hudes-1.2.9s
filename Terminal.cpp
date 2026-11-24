#include "pch.h"
#include "Terminal.h"
#include <algorithm>
#include <array>
#include <sstream>

namespace fs = std::filesystem;
namespace
{
    std::wstring Env(wchar_t const* name)
    {
        auto n = GetEnvironmentVariableW(name, nullptr, 0);
        if (!n) return {};
        std::wstring result(n, L'\0');
        result.resize(GetEnvironmentVariableW(name, result.data(), n));
        return result;
    }

    fs::path OnPath(std::wstring const& file)
    {
        std::wistringstream paths(Env(L"PATH"));
        for (std::wstring entry; std::getline(paths, entry, L';');)
        {
            if (entry.size() > 1 && entry.front() == L'"' && entry.back() == L'"') entry = entry.substr(1, entry.size()-2);
            fs::path folder(entry);
            if (!folder.is_absolute()) continue;
            std::error_code ec;
            if (fs::is_regular_file(folder/file, ec)) return folder/file;
        }
        return {};
    }

    std::wstring RegText(HKEY key, wchar_t const* subkey, wchar_t const* name)
    {
        DWORD bytes{};
        if (RegGetValueW(key, subkey, name, RRF_RT_REG_SZ, nullptr, nullptr, &bytes) || !bytes) return {};
        std::wstring value(bytes / sizeof(wchar_t), L'\0');
        if (RegGetValueW(key, subkey, name, RRF_RT_REG_SZ, nullptr, value.data(), &bytes)) return {};
        value.resize(wcsnlen_s(value.data(), value.size()));
        return value;
    }

    std::wstring QuoteArgument(std::wstring const& argument)
    {
        std::wstring result = L"\"";
        size_t slashes{};
        for (auto c : argument)
        {
            if (c == L'\\') { ++slashes; continue; }
            result.append(c == L'"' ? 2 * slashes + 1 : slashes, L'\\');
            result += c;
            slashes = 0;
        }
        result.append(2*slashes, L'\\');
        return result + L'"';
    }
}
namespace HudesNative
{
    std::vector<ShellProfile> InstalledShells()
    {
        std::vector<ShellProfile> profiles;
        auto add = [&](std::wstring id, std::wstring label, fs::path exe, std::wstring args = {})
        {
            std::error_code ec;
            if (exe.is_absolute() && fs::is_regular_file(exe, ec))
                profiles.push_back({std::move(id), std::move(label), std::move(exe), std::move(args)});
        };
        std::array<wchar_t, MAX_PATH> sys{};
        GetSystemDirectoryW(sys.data(), static_cast<UINT>(sys.size()));
        fs::path system(sys.data());
        add(L"cmd", L"Command Prompt", system/L"cmd.exe", L"/D");
        add(L"windows-powershell", L"Windows PowerShell", system/L"WindowsPowerShell/v1.0/powershell.exe", L"-NoLogo");
        auto pwsh = OnPath(L"pwsh.exe");
        add(L"pwsh", L"PowerShell 7+", pwsh.empty() ? fs::path(Env(L"ProgramFiles"))/L"PowerShell/7/pwsh.exe" : pwsh, L"-NoLogo");
        auto git = fs::path(RegText(HKEY_CURRENT_USER, L"SOFTWARE\\GitForWindows", L"InstallPath"));
        if (git.empty()) git = RegText(HKEY_LOCAL_MACHINE, L"SOFTWARE\\GitForWindows", L"InstallPath");
        if (git.empty()) git = OnPath(L"git.exe").parent_path().parent_path();
        if (git.empty()) git = fs::path(Env(L"ProgramFiles"))/L"Git";
        add(L"git-bash", L"Git Bash", git/L"bin/bash.exe", L"-i");
        add(L"nu", L"Nushell", OnPath(L"nu.exe"));
        wil::unique_hkey lxss;
        if (!RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Lxss", 0, KEY_READ, lxss.put()))
        {
            for (DWORD i=0;; ++i)
            {
                wchar_t key[256]{};
                DWORD count = ARRAYSIZE(key);
                if (RegEnumKeyExW(lxss.get(), i, key, &count, nullptr, nullptr, nullptr, nullptr)) break;
                auto name = RegText(lxss.get(), key, L"DistributionName");
                if (!name.empty()) add(L"wsl/"+name, L"WSL · "+name, system/L"wsl.exe", L"--distribution "+QuoteArgument(name));
            }
        }
        return profiles;
    }

    TerminalSession::TerminalSession(ShellProfile const& shell, fs::path const& directory, COORD size, Output output)
    {
        wil::unique_handle consoleInput, consoleOutput;
        winrt::check_bool(CreatePipe(consoleInput.put(), m_input.put(), nullptr, 0));
        winrt::check_bool(CreatePipe(m_output.put(), consoleOutput.put(), nullptr, 0));
        winrt::check_hresult(CreatePseudoConsole(size, consoleInput.get(), consoleOutput.get(), 0, &m_console));
        auto cleanup = wil::scope_exit([&] { CloseConsole(); });
        m_inputReady.create();
        consoleInput.reset(); consoleOutput.reset();

        SIZE_T bytes{};
        InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        std::vector<unsigned char> storage(bytes);
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        winrt::check_bool(InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &bytes));
        auto release = wil::scope_exit([&] { DeleteProcThreadAttributeList(startup.lpAttributeList); });
        winrt::check_bool(UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
            m_console, sizeof(m_console), nullptr, nullptr));
        std::wstring command = QuoteArgument(shell.executable.wstring()) + L" " + shell.arguments;
        if (shell.id.starts_with(L"wsl/")) command += L" --cd " + QuoteArgument(directory.wstring());
        // CMD needs a temporary drive mapping for UNC working directories. Supply the directory as
        // an environment value so a filename never becomes command syntax.
        std::vector<wchar_t> environment;
        if (shell.id == L"cmd" && directory.wstring().starts_with(L"\\\\"))
        {
            auto block = GetEnvironmentStringsW();
            winrt::check_pointer(block);
            auto freeBlock = wil::scope_exit([&] { FreeEnvironmentStringsW(block); });
            std::vector<std::wstring> vars;
            for (auto p=block; *p; p+=wcslen(p)+1)
                if (_wcsnicmp(p,L"HUDES_TERMINAL_DIR=",18)) vars.emplace_back(p);
            vars.emplace_back(L"HUDES_TERMINAL_DIR="+directory.wstring());
            std::ranges::sort(vars, [](auto const& a,auto const& b){ return _wcsicmp(a.c_str(),b.c_str())<0; });
            for(auto const& v:vars) environment.insert(environment.end(),v.c_str(),v.c_str()+v.size()+1);
            environment.push_back(0);
            command += L" /V:OFF /K pushd \"%HUDES_TERMINAL_DIR%\"";
        }
        m_job.reset(CreateJobObjectW(nullptr, nullptr));
        winrt::check_bool(bool(m_job));
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        winrt::check_bool(SetInformationJobObject(m_job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)));
        PROCESS_INFORMATION process{};
        winrt::check_bool(CreateProcessW(shell.executable.c_str(), command.data(), nullptr, nullptr, FALSE,
            EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
            environment.empty()?nullptr:environment.data(), directory.c_str(), &startup.StartupInfo, &process));
        m_process.reset(process.hProcess);
        wil::unique_handle thread(process.hThread);
        auto abort = wil::scope_exit([&] { TerminateProcess(m_process.get(), 1); });
        winrt::check_bool(AssignProcessToJobObject(m_job.get(), m_process.get()));
        winrt::check_bool(ResumeThread(thread.get()) != DWORD(-1));
        m_reader = std::thread([this, output = std::move(output)]
        {
            char buffer[16384];
            DWORD count{};
            try
            {
                while (ReadFile(m_output.get(), buffer, sizeof(buffer), &count, nullptr) && count)
                    output(std::string(buffer, count));
                output({});
            }
            catch (...) {}
        });
        try
        {
            m_writer = std::thread([this]
            {
                HANDLE waits[]{m_process.get(), m_inputReady.get()};
                while (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0 + 1)
                {
                    std::string data;
                    {
                        std::lock_guard lock(m_mutex);
                        if (m_stopping) break;
                        if (m_queue.empty()) continue;
                        data = std::move(m_queue.front());
                        m_queue.pop_front();
                        if (!m_queue.empty()) m_inputReady.SetEvent();
                    }
                    DWORD written{};
                    size_t offset{};
                    while (offset < data.size() && WriteFile(m_input.get(), data.data() + offset,
                        static_cast<DWORD>((std::min)(data.size() - offset, size_t{65536})), &written, nullptr) && written)
                        offset += written;
                    if (offset != data.size()) break;
                }
                // Closing the pseudoconsole lets the reader deliver its final output and EOF.
                CloseConsole();
            });
        }
        catch (...)
        {
            TerminateProcess(m_process.get(), 1);
            CloseConsole();
            m_reader.join();
            throw;
        }
        abort.release();
        cleanup.release();
    }

    void TerminalSession::CloseConsole() noexcept
    {
        HPCON console{};
        {
            std::lock_guard lock(m_mutex);
            m_stopping = true;
            m_queue.clear();
            console = std::exchange(m_console, nullptr);
        }
        if (console) ClosePseudoConsole(console);
    }

    TerminalSession::~TerminalSession()
    {
        {
            std::lock_guard lock(m_mutex);
            m_stopping = true;
        }
        m_inputReady.SetEvent();
        m_job.reset();
        if (m_writer.joinable()) CancelSynchronousIo(m_writer.native_handle());
        CloseConsole();
        if (m_writer.joinable()) m_writer.join();
        if (m_reader.joinable()) m_reader.join();
    }

    void TerminalSession::Write(std::string input)
    {
        if (input.empty()) return;
        std::lock_guard lock(m_mutex);
        if (m_stopping) return;
        m_queue.push_back(std::move(input));
        m_inputReady.SetEvent();
    }

    void TerminalSession::Resize(COORD size)
    {
        std::lock_guard lock(m_mutex);
        if (m_console) ResizePseudoConsole(m_console, size);
    }
}

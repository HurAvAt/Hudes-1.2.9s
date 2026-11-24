#pragma once

#include <windows.h>
#include <wil/resource.h>
#include <atomic>
#include <utility>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace HudesNative
{
    struct ShellProfile
    {
        std::wstring id, label;
        std::filesystem::path executable;
        std::wstring arguments;
    };
    std::vector<ShellProfile> InstalledShells();
    class TerminalSession final
    {
    public:
        using Output = std::function<void(std::string)>;
        TerminalSession(ShellProfile const&, std::filesystem::path const&, COORD, Output);
        ~TerminalSession();
        TerminalSession(TerminalSession const&) = delete;
        void Write(std::string input);
        void Resize(COORD size);
    private:
        wil::unique_handle m_input, m_output, m_process, m_job;
        HPCON m_console{};
        std::mutex m_mutex;
        wil::unique_event m_inputReady;
        void CloseConsole() noexcept;
        std::deque<std::string> m_queue;
        bool m_stopping{};
        std::thread m_reader, m_writer;
    };
}

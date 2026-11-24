#include "pch.h"
#include "App.xaml.h"
#include "MainWindow.xaml.h"
#include "CoreTests.h"
#include <shellapi.h>

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;

namespace winrt::HudesNative::implementation
{
    App::App()
    {
        SetLastError(ERROR_SUCCESS);
        m_instanceMutex.reset(CreateMutexW(nullptr, FALSE, L"Local\\Hudes-1.2.9s-Donate-SingleInstance"));
        m_anotherInstance = m_instanceMutex && GetLastError() == ERROR_ALREADY_EXISTS;
    }

    void App::OnLaunched(LaunchActivatedEventArgs const&)
    {
        int count{};
        auto arguments = CommandLineToArgvW(GetCommandLineW(), &count);
        auto selfTest = arguments && count == 2 && _wcsicmp(arguments[1], L"--self-test") == 0;
        auto checkResources = arguments && count == 2 && _wcsicmp(arguments[1], L"--resource-test") == 0;
        if (arguments) LocalFree(arguments);
        if (checkResources) ExitProcess(::HudesNative::RunResourceTests() ? 0 : 1);
        if (selfTest)
        {
            auto passed = ::HudesNative::RunCoreTests();
            ExitProcess(passed ? 0 : 1);
        }
        if (m_anotherInstance)
        {
            if (auto window = FindWindowW(nullptr, L"Hudes 1.2.9s – Donate"))
            {
                ShowWindow(window, SW_RESTORE);
                SetForegroundWindow(window);
            }
            Exit();
            return;
        }
        try
        {
            m_window = make<MainWindow>();
            m_window.Activate();
        }
        catch (...)
        {
            auto message = L"Hudes could not open.\n\n" + ::HudesNative::FriendlyError();
            MessageBoxW(nullptr, message.c_str(), L"Hudes", MB_OK | MB_ICONERROR);
            Exit();
        }
    }
}

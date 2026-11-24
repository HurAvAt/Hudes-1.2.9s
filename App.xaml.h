#pragma once

#include "App.xaml.g.h"
#include "NativeServices.h"

namespace winrt::HudesNative::implementation
{
    struct App : AppT<App>
    {
        App();
        void OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&);

    private:
        Microsoft::UI::Xaml::Window m_window{ nullptr };
        ::HudesNative::unique_handle m_instanceMutex;
        bool m_anotherInstance{};
    };
}

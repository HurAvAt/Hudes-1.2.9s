#include "pch.h"
#include "MainWindow.xaml.h"
#include <winrt/Microsoft.Web.WebView2.Core.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Security.Cryptography.h>
#include <algorithm>
#include <shlobj.h>

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::Web::WebView2::Core;
using namespace winrt::Windows::Data::Json;
using namespace winrt::Windows::Security::Cryptography;
namespace fs = std::filesystem;

namespace winrt::HudesNative::implementation
{
    void MainWindow::SetupTerminal()
    {
        m_terminalTimer = DispatcherTimer();
        m_terminalTimer.Interval(std::chrono::seconds(2));
        m_terminalTimer.Tick([this](auto const&, auto const&)
        {
            m_terminalTimer.Stop();
            if (!m_terminalHere && !m_shellMenuOpen && !m_splitterDragging) CloseTerminalPanel();
        });
        TerminalPanel().PointerEntered([this](auto const&,auto const&){ m_terminalTimer.Stop(); });
        TerminalPanel().PointerExited([this](auto const&,auto const&)
        {
            if (m_store && m_terminalOpen && !m_terminalHere && !m_store->Settings().terminalPinned) m_terminalTimer.Start();
        });
        TerminalRail().Click([this](auto const&,auto const&){ OpenTerminal(); });
        CloseTerminal().Click([this](auto const&,auto const&){ CloseTerminalPanel(true); });
        ShellMenu().Opened([this](auto const&,auto const&) { m_shellMenuOpen=true; m_terminalTimer.Stop(); });
        ShellMenu().Closed([this](auto const&,auto const&) { m_shellMenuOpen=false; });
        EndShell().Click([this](auto const&,auto const&){ RemoveShell(m_activeShell); });
        TerminalTabs().SelectionChanged([this](auto const&,auto const&)
        {
            if(m_switchingShell) return;
            auto item=TerminalTabs().SelectedItem().try_as<ListViewItem>();
            if(item) SelectShell(unbox_value<uint32_t>(item.Tag()));
        });
        PinTerminal().Click([this](auto const&,auto const&)
        {
            try
            {
                auto& settings=m_store->Settings();
                settings.terminalPinned=PinTerminal().IsChecked().Value();
                m_terminalHere=false;
                m_store->Save(); UpdateTerminalPanel();
            } catch (...) { ShowMessage(L"Terminal",::HudesNative::FriendlyError()); }
        });
    }

    void MainWindow::LoadTerminalSettings()
    {
        auto const& settings=m_store->Settings();
        ShowTerminalCheck().IsChecked(settings.terminalEnabled);
        m_shells=::HudesNative::InstalledShells();
        ShellChoice().Items().Clear();
        ShellMenu().Items().Clear();
        for(auto const& shell:m_shells)
        {
            MenuFlyoutItem choice;
            choice.Text(shell.label);
            choice.Click([weak=get_weak(),id=shell.id](auto const&,auto const&)
            { if (auto self=weak.get(); self && self->m_terminalReady) self->StartTerminalSession(id); });
            ShellMenu().Items().Append(choice);
            ComboBoxItem item; item.Content(box_value(shell.label)); item.Tag(box_value(shell.id));
            ShellChoice().Items().Append(item);
            if(shell.id==settings.terminalShell) ShellChoice().SelectedItem(item);
        }
        if(ShellChoice().SelectedIndex()<0 && !m_shells.empty()) ShellChoice().SelectedIndex(0);
        UpdateTerminalPanel();
    }

    void MainWindow::UpdateTerminalPanel()
    {
        if(!m_store) return;
        auto const& settings=m_store->Settings();
        AddShell().IsEnabled(m_terminalReady && !m_shells.empty());
        bool visible=m_terminalOpen || (settings.terminalEnabled && settings.terminalPinned);
        TerminalPanel().Visibility(visible?Visibility::Visible:Visibility::Collapsed);
        TerminalRail().Visibility(settings.terminalEnabled && !visible?Visibility::Visible:Visibility::Collapsed);
        PinTerminal().IsChecked(settings.terminalEnabled && settings.terminalPinned);
        PinTerminal().IsEnabled(settings.terminalEnabled);
        if(!visible || settings.terminalPinned) m_terminalTimer.Stop();
        if(visible && m_ready && !m_terminalOpen) OpenTerminal();
    }

    void MainWindow::OpenTerminal(fs::path folder,bool here)
    {
        if(!m_ready || m_windowClosed) return;
        if(folder.empty()) folder=IsArchiveFolder(m_currentFolder)?m_desktop:m_currentFolder;
        if(folder.empty()) folder=m_desktop;
        m_terminalOpen=true;
        m_terminalHere=here;
        m_terminalTimer.Stop();
        m_terminalFolder=std::move(folder);
        UpdateTerminalPanel();
        if(!m_terminalReady){ InitializeTerminalView(); return; }
        if(here || !m_activeShell) StartTerminalSession();
        else SelectShell(m_activeShell);
    }

    void MainWindow::CloseTerminalPanel(bool explicitClose)
    {
        if(!m_store || !m_terminalOpen) return;
        auto& settings=m_store->Settings();
        if(settings.terminalEnabled && settings.terminalPinned)
        {
            if(!explicitClose) return;
            settings.terminalPinned=false;
            try {m_store->Save();} catch (...) {ShowMessage(L"Terminal",::HudesNative::FriendlyError());}
        }
        m_terminalOpen=false; m_terminalHere=false; m_terminalTimer.Stop();
        if(!settings.terminalEnabled) EndTerminalSession();
        UpdateTerminalPanel();
    }

    void MainWindow::TerminalMessage(unsigned id,std::wstring const& type,std::wstring const& value)
    {
        if(!m_terminalReady || !m_terminalWeb || m_windowClosed) return;
        JsonObject message;
        message.Insert(L"id",JsonValue::CreateNumberValue(id));
        message.Insert(L"type",JsonValue::CreateStringValue(type));
        message.Insert(L"value",JsonValue::CreateStringValue(value));
        m_terminalWeb.CoreWebView2().PostWebMessageAsJson(message.Stringify());
    }

    void MainWindow::TerminalStyle()
    {
        if(!m_terminalReady) return;
        auto const& cfg=m_store->Settings();
        JsonObject theme, options;
        theme.Insert(L"background",JsonValue::CreateStringValue(cfg.Look().Color(L"TerminalBrush")));
        theme.Insert(L"foreground",JsonValue::CreateStringValue(cfg.Look().fonts[::HudesNative::ShellFont].color));
        theme.Insert(L"cursor",JsonValue::CreateStringValue(cfg.Look().fonts[::HudesNative::ShellFont].color));
        options.Insert(L"theme",theme);
        options.Insert(L"fontSize",JsonValue::CreateNumberValue(cfg.Look().fonts[::HudesNative::ShellFont].size));
        options.Insert(L"fontFamily",JsonValue::CreateStringValue(cfg.Look().fonts[::HudesNative::ShellFont].family+L", monospace"));
        TerminalMessage(0,L"style",std::wstring(options.Stringify()));
    }

    fire_and_forget MainWindow::InitializeTerminalView()
    {
        if(m_terminalStarting || m_terminalReady || m_terminalWeb) co_return;
        auto lifetime=get_strong();
        m_terminalStarting=true;
        auto finished=wil::scope_exit([&]{m_terminalStarting=false;});
        try
        {
            TerminalNotice().Text(L"Opening terminal…");
            m_terminalWeb=WebView2();
            TerminalSurface().Children().InsertAt(0,m_terminalWeb);
            wil::unique_cotaskmem_string local;
            check_hresult(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,local.put()));
            auto cache=fs::path(local.get())/L"Hudes"/L"TerminalWebView";
            auto env=co_await CoreWebView2Environment::CreateWithOptionsAsync(L"",cache.wstring(),nullptr);
            if(m_windowClosed) co_return;
            co_await m_terminalWeb.EnsureCoreWebView2Async(env);
            if(m_windowClosed) co_return;
            auto core=m_terminalWeb.CoreWebView2();
            auto assets=::HudesNative::ExecutableFolder()/L"Assets"/L"Terminal";
            if(!fs::is_regular_file(assets/L"index.html")) throw hresult_error(E_FAIL,L"The terminal display files are missing.");
            core.SetVirtualHostNameToFolderMapping(L"terminal.hudes.invalid",assets.wstring(),CoreWebView2HostResourceAccessKind::Deny);
            core.Settings().AreDevToolsEnabled(false);
            core.Settings().AreDefaultContextMenusEnabled(false);
            core.Settings().AreHostObjectsAllowed(false);
            core.Settings().IsStatusBarEnabled(false);
            core.Settings().IsZoomControlEnabled(false);
            core.NavigationStarting([](auto const&,CoreWebView2NavigationStartingEventArgs const& args)
            { args.Cancel(args.Uri()!=L"https://terminal.hudes.invalid/index.html"); });
            core.NavigationCompleted([weak=get_weak()](auto const&,CoreWebView2NavigationCompletedEventArgs const& args)
            {
                if(!args.IsSuccess()) if(auto self=weak.get()) self->TerminalNotice().Text(L"Could not load the local terminal page. Extract the complete Hudes ZIP and reopen the app.");
            });
            core.NewWindowRequested([](auto const&,CoreWebView2NewWindowRequestedEventArgs const& args){args.Handled(true);});
            core.PermissionRequested([](auto const&,CoreWebView2PermissionRequestedEventArgs const& args){args.State(CoreWebView2PermissionState::Deny);});
            core.WebMessageReceived([weak=get_weak()](auto const&,CoreWebView2WebMessageReceivedEventArgs const& args)
            {
                auto self=weak.get();
                if(!self || self->m_windowClosed || args.Source()!=L"https://terminal.hudes.invalid/index.html") return;
                try
                {
                    auto message=JsonObject::Parse(args.TryGetWebMessageAsString());
                    auto type=message.GetNamedString(L"type");
                    if(type==L"ready")
                    {
                        self->m_terminalReady=true;
                        self->AddShell().IsEnabled(!self->m_shells.empty());
                        self->TerminalNotice().Text(L"");
                        self->TerminalStyle();
                        if(self->m_terminalOpen && self->m_ready) self->StartTerminalSession();
                        return;
                    }
                    if(!self->m_ready || !self->m_terminalOpen) return;
                    if(type==L"inside"){self->OutsideNotebook(); return;}
                    if(type==L"enter"){self->m_terminalTimer.Stop(); return;}
                    if(type==L"leave"){if(!self->m_terminalHere && !self->m_store->Settings().terminalPinned) self->m_terminalTimer.Start(); return;}
                    auto id=static_cast<unsigned>(message.GetNamedNumber(L"id"));
                    auto tab=self->m_shellTabs.find(id);
                    if(tab==self->m_shellTabs.end() || !tab->second.session || self->m_activeShell!=id) return;
                    auto value=message.GetNamedString(L"value");
                    if(type==L"input") tab->second.session->Write(to_string(value));
                    if(type==L"size")
                    {
                        int cols{},rows{};
                        if(swscanf_s(value.c_str(),L"%d,%d",&cols,&rows)==2)
                        {
                            self->m_terminalSize={static_cast<SHORT>(std::clamp(cols,2,1000)),static_cast<SHORT>(std::clamp(rows,1,400))};
                            tab->second.session->Resize(self->m_terminalSize);
                        }
                    }
                } catch (...) {}
            });
            core.Navigate(L"https://terminal.hudes.invalid/index.html");
        }
        catch (...)
        {
            TerminalNotice().Text(L"Could not open the terminal. Check that Microsoft Edge WebView2 Runtime is available. "+::HudesNative::FriendlyError());
            if(m_terminalWeb) {uint32_t i{}; if(TerminalSurface().Children().IndexOf(m_terminalWeb,i)) TerminalSurface().Children().RemoveAt(i); m_terminalWeb.Close(); m_terminalWeb=nullptr;}
        }
    }

    void MainWindow::StartTerminalSession(std::wstring profile)
    {
        try
        {
            if(m_shells.empty()) throw hresult_error(E_FAIL,L"No supported shell was found.");
            if (profile.empty()) profile=m_store->Settings().terminalShell;
            auto shell=std::ranges::find(m_shells,profile,&::HudesNative::ShellProfile::id);
            if(shell==m_shells.end()) shell=m_shells.begin();
            auto id=++m_nextShell;
            auto queue=DispatcherQueue();
            auto weak=get_weak();
            auto session=std::make_shared<::HudesNative::TerminalSession>(*shell,m_terminalFolder,m_terminalSize,
                [weak,queue,id](std::string data)
                {
                    // Backpressure prevents a fast command from filling the UI dispatcher with output.
                    auto ack=std::make_shared<wil::unique_event>(); ack->create();
                    if(!queue.TryEnqueue([weak,id,data=std::move(data),ack]
                    {
                        auto release=wil::scope_exit([&]{ack->SetEvent();});
                        try
                        {
                            auto self=weak.get();
                            if(!self || !self->m_shellTabs.contains(id)) return;
                            if(data.empty())
                            {
                                auto ended = std::move(self->m_shellTabs.at(id).session);
                                if (ended) std::thread([ended = std::move(ended)] {}).detach();
                                self->TerminalMessage(id,L"end");
                                return;
                            }
                            auto raw=CryptographicBuffer::CreateFromByteArray(array_view(reinterpret_cast<uint8_t const*>(data.data()),reinterpret_cast<uint8_t const*>(data.data()+data.size())));
                            self->TerminalMessage(id,L"out",std::wstring(CryptographicBuffer::EncodeToBase64String(raw)));
                        } catch (...) {}
                    })) return;
                    WaitForSingleObject(ack->get(),2000);
                });
            m_shellTabs.emplace(id,ShellTab{std::move(session),m_terminalFolder,shell->id});
            TerminalNotice().Text(L"");
            TerminalMessage(id,L"add");
            ListViewItem item;
            item.Tag(box_value(id)); item.Content(box_value(shell->label+L"  ·  "+std::to_wstring(id)));
            m_switchingShell=true;
            TerminalTabs().Items().Append(item); TerminalTabs().SelectedItem(item);
            m_switchingShell=false;
            SelectShell(id);
        }
        catch (...) { TerminalNotice().Text(L"Could not start the shell. "+::HudesNative::FriendlyError()); }
    }

    void MainWindow::SelectShell(unsigned id)
    {
        auto tab=m_shellTabs.find(id); if(tab==m_shellTabs.end()) return;
        m_activeShell=id;
        TerminalLocation().Text(tab->second.folder.wstring());
        ToolTipService::SetToolTip(TerminalLocation(),box_value(L"Starting folder: "+tab->second.folder.wstring()));
        TerminalMessage(id,L"select");
        if(m_terminalWeb) m_terminalWeb.Focus(FocusState::Programmatic);
    }

    void MainWindow::RemoveShell(unsigned id)
    {
        auto tab=m_shellTabs.find(id); if(tab==m_shellTabs.end()) return;
        auto session=std::move(tab->second.session);
        m_shellTabs.erase(tab);
        std::thread([session=std::move(session)]{}).detach();
        TerminalMessage(id,L"close");
        m_switchingShell=true;
        for(uint32_t i=0;i<TerminalTabs().Items().Size();++i)
            if(unbox_value<uint32_t>(TerminalTabs().Items().GetAt(i).as<ListViewItem>().Tag())==id)
                {TerminalTabs().Items().RemoveAt(i); break;}
        m_switchingShell=false;
        if(m_shellTabs.empty())
        {
            m_activeShell=0; TerminalLocation().Text(L"");
            if(!m_windowClosed && m_terminalOpen) CloseTerminalPanel(true);
        }
        else
        {
            TerminalTabs().SelectedIndex(static_cast<int>(TerminalTabs().Items().Size()-1));
            if(auto item=TerminalTabs().SelectedItem().try_as<ListViewItem>()) SelectShell(unbox_value<uint32_t>(item.Tag()));
        }
    }

    void MainWindow::EndTerminalSession()
    {
        m_terminalOpen=false; m_terminalHere=false;
        m_terminalTimer.Stop();
        auto sessions=std::move(m_shellTabs);
        m_shellTabs.clear();
        for(auto const& [id,tab]:sessions) TerminalMessage(id,L"close");
        if(!sessions.empty()) std::thread([sessions=std::move(sessions)]{}).detach();
        m_switchingShell=true; TerminalTabs().Items().Clear(); m_switchingShell=false;
        m_activeShell=0; TerminalLocation().Text(L"");
    }

    void MainWindow::AppendViewMenu(MenuFlyout const& menu,fs::path location)
    {
        MenuFlyoutItem terminal;
        terminal.Text(L"Terminal here");
        terminal.Click([weak=get_weak(),location=std::move(location)](auto const&,auto const&)
        {if(auto self=weak.get()) self->OpenTerminal(location,true);});
        menu.Items().Append(terminal);
        ToggleMenuFlyoutItem hidden;
        hidden.Text(L"Show hidden files"); hidden.IsChecked(m_store->Settings().showHidden);
        hidden.Click([weak=get_weak()](Windows::Foundation::IInspectable const& sender,auto const&)
        {
            if(auto self=weak.get()) try {
                self->m_store->Settings().showHidden=sender.as<ToggleMenuFlyoutItem>().IsChecked();
                self->m_store->Save(); self->RefreshFiles(); self->RefreshSearch(false);
            } catch (...) { self->ShowMessage(L"Hidden files",::HudesNative::FriendlyError()); }
        });
        menu.Items().Append(hidden);
    }
}

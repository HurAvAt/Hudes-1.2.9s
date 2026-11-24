#include "pch.h"
#include "MainWindow.xaml.h"
#include <algorithm>
#include <cmath>

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Controls::Primitives;
using namespace winrt::Microsoft::UI::Xaml::Input;
using namespace winrt::Microsoft::UI::Xaml::Media;
using namespace winrt::Windows::Foundation;

namespace
{
    bool Within(DependencyObject element, DependencyObject const& parent)
    {
        for (; element; element = VisualTreeHelper::GetParent(element))
            if (element == parent) return true;
        return false;
    }
}

namespace winrt::HudesNative::implementation
{
    void MainWindow::CompactWindow(bool compact)
    {
        if (compact == m_compact) return;
        auto handle = WindowHandle();
        if (compact)
        {
            if (!GetWindowPlacement(handle, &m_workspacePlacement)) return;
            if (m_workspacePlacement.showCmd == SW_HIDE) m_workspacePlacement.showCmd = SW_SHOWNORMAL;
            if (IsZoomed(handle)) ShowWindow(handle, SW_RESTORE);
            MONITORINFO monitor{ sizeof(MONITORINFO) };
            GetMonitorInfoW(MonitorFromWindow(handle, MONITOR_DEFAULTTONEAREST), &monitor);
            auto work = monitor.rcWork;
            auto dpi = static_cast<int>(GetDpiForWindow(handle));
            int width = (std::min)(MulDiv(412, dpi, 96), static_cast<int>(work.right - work.left));
            int height = (std::min)(MulDiv(440, dpi, 96), static_cast<int>(work.bottom - work.top));
            SetWindowPos(handle, nullptr, work.left + (work.right - work.left - width) / 2,
                work.top + (work.bottom - work.top - height) / 2, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        else SetWindowPlacement(handle, &m_workspacePlacement);
        m_compact = compact;
    }

    void MainWindow::EnableEditorClickAway(ContentDialog const& dialog)
    {
        auto outsideTap = std::make_shared<UIElement::Tapped_revoker>();
        dialog.Closed([outsideTap](auto const&, auto const&) { outsideTap->revoke(); });
        dialog.Opened([outsideTap](ContentDialog const& sender, auto const&)
        {
            for (auto const& popup : VisualTreeHelper::GetOpenPopupsForXamlRoot(sender.XamlRoot()))
            {
                std::vector<DependencyObject> pending{ popup.Child() };
                while (!pending.empty())
                {
                    auto node = pending.back();
                    pending.pop_back();
                    if (!node) continue;
                    if (auto element = node.try_as<FrameworkElement>(); element && element.Name() == L"SmokeLayerBackground")
                    {
                        *outsideTap = element.Tapped(auto_revoke, [weak = make_weak(sender)](auto const&, TappedRoutedEventArgs const& args)
                        {
                            args.Handled(true);
                            if (auto owner = weak.get()) owner.Hide();
                        });
                        return;
                    }
                    for (int i = 0; i < VisualTreeHelper::GetChildrenCount(node); ++i)
                        pending.push_back(VisualTreeHelper::GetChild(node, i));
                }
            }
        });
    }

    Windows::Foundation::IAsyncOperation<ContentDialogResult> MainWindow::ShowLightEditor(
        ContentDialog dialog, FrameworkElement anchor, std::function<bool()> dirty, std::function<bool()> save)
    {
        auto lifetime=get_strong();
        Flyout flyout;
        StackPanel layout; layout.Spacing(10);
        TextBlock title; title.Text(unbox_value_or<hstring>(dialog.Title(),L"Edit"));
        title.FontSize(19); title.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
        layout.Children().Append(title);
        auto fields=dialog.Content().as<FrameworkElement>(); dialog.Content(nullptr);
        if(auto scroll=fields.try_as<ScrollViewer>()) scroll.MaxHeight((std::min)(425.0,(std::max)(160.0,RootGrid().ActualHeight()-230)));
        layout.Children().Append(fields);
        StackPanel actions; actions.Orientation(Orientation::Horizontal); actions.Spacing(7);
        Button primary,secondary,close;
        auto sync=[&]{primary.Content(box_value(dialog.PrimaryButtonText()));primary.IsEnabled(dialog.IsPrimaryButtonEnabled());};
        sync();
        auto textWatch=dialog.RegisterPropertyChangedCallback(ContentDialog::PrimaryButtonTextProperty(),[&](auto const&,auto const&){sync();});
        auto enabledWatch=dialog.RegisterPropertyChangedCallback(ContentDialog::IsPrimaryButtonEnabledProperty(),[&](auto const&,auto const&){sync();});
        auto unwatch=wil::scope_exit([&]{dialog.UnregisterPropertyChangedCallback(ContentDialog::PrimaryButtonTextProperty(),textWatch);dialog.UnregisterPropertyChangedCallback(ContentDialog::IsPrimaryButtonEnabledProperty(),enabledWatch);});
        primary.Background(UiBrush(L"SelectedBrush")); actions.Children().Append(primary);
        ContentDialogResult result=ContentDialogResult::None;
        bool accepted=false;
        auto commit=[&]{if(!dialog.IsPrimaryButtonEnabled() || !save()) return false; result=ContentDialogResult::Primary; return true;};
        auto onPrimary = primary.Click(auto_revoke, [&](auto const&,auto const&){if(commit()){accepted=true;flyout.Hide();}});
        Button::Click_revoker onSecondary;
        if(!dialog.SecondaryButtonText().empty())
        {
            secondary.Content(box_value(dialog.SecondaryButtonText()));
            onSecondary = secondary.Click(auto_revoke, [&](auto const&,auto const&){result=ContentDialogResult::Secondary;accepted=true;flyout.Hide();});
            actions.Children().Append(secondary);
        }
        close.Content(box_value(dialog.CloseButtonText()));auto onClose = close.Click(auto_revoke, [&](auto const&,auto const&){flyout.Hide();});
        actions.Children().Append(close);layout.Children().Append(actions);flyout.Content(layout);
        wil::unique_event closed;closed.create();
        auto onClosing = flyout.Closing(auto_revoke, [&](auto const&,FlyoutBaseClosingEventArgs const& args)
        {
            if(accepted || !dirty()) return;
            int choice=AskToSave(L"this editor");
            args.Cancel(choice==IDCANCEL || (choice==IDYES && !commit()));
        });
        auto onDismiss = flyout.Closed(auto_revoke,[&](auto const&,auto const&){closed.SetEvent();});
        auto onWindowClose = Closed(auto_revoke,[&](auto const&,auto const&){closed.SetEvent();});
        auto ui=apartment_context();flyout.ShowAt(anchor);
        co_await resume_on_signal(closed.get());co_await ui;
        co_return m_windowClosed ? ContentDialogResult::None : result;
    }

    void MainWindow::SetDropHighlight(Border const& folder)
    {
        if (m_hoverFolder == folder) return;
        if (m_hoverFolder) m_hoverFolder.BorderThickness({ 0 });
        m_hoverFolder = folder;
        if (!folder) return;
        folder.BorderBrush(UiBrush(L"AccentBrush"));
        folder.BorderThickness({ 2 });
    }

    void MainWindow::SetupItemMove()
    {
        auto surface = FileSurface();
        auto itemAt = [this](Point point) -> Border
        {
            auto list = ActiveFileList();
            if (auto panel = list.ItemsPanelRoot())
                for (auto const& child : panel.Children())
                {
                    auto item = list.ItemFromContainer(child).try_as<Border>();
                    if (!item) continue;
                    auto bounds = item.TransformToVisual(FileSurface()).TransformBounds({ 0, 0,
                        static_cast<float>(item.ActualWidth()), static_cast<float>(item.ActualHeight()) });
                    if (point.X >= bounds.X && point.X <= bounds.X + bounds.Width &&
                        point.Y >= bounds.Y && point.Y <= bounds.Y + bounds.Height) return item;
                }
            return nullptr;
        };
        surface.PointerEntered([this](auto const&, PointerRoutedEventArgs const& args)
        {
            bool native = args.Pointer().PointerDeviceType() != Microsoft::UI::Input::PointerDeviceType::Mouse;
            FileGrid().CanDragItems(native);
            FileDetails().CanDragItems(native);
        });
        surface.AddHandler(UIElement::PointerPressedEvent(), box_value(PointerEventHandler([this, itemAt](auto const&, PointerRoutedEventArgs const& args)
        {
            m_itemDragOrigin.reset();
            if (!m_ready || args.Pointer().PointerDeviceType() != Microsoft::UI::Input::PointerDeviceType::Mouse) return;
            auto pt = args.GetCurrentPoint(FileSurface());
            if (!pt.Properties().IsLeftButtonPressed()) return;
            auto item = itemAt(pt.Position());
            if (!item) return;
            auto path = std::filesystem::path(unbox_value_or<hstring>(item.Tag(), L"").c_str());
            if (!CanManage(path)) return;
            m_itemDragOrigin = pt.Position();
            m_itemDragFiles = SelectedPaths();
            if (std::ranges::find(m_itemDragFiles, path) == m_itemDragFiles.end()) m_itemDragFiles = { path };
        })), true);
        surface.AddHandler(UIElement::PointerMovedEvent(), box_value(PointerEventHandler([this, itemAt](auto const&, PointerRoutedEventArgs const& args)
        {
            if (!m_itemDragOrigin) return;
            auto pt = args.GetCurrentPoint(FileSurface());
            if (!pt.Properties().IsLeftButtonPressed()) { m_itemDragOrigin.reset(); SetDropHighlight(nullptr); return; }
            auto point = pt.Position();
            if (std::abs(point.X - m_itemDragOrigin->X) < 5 && std::abs(point.Y - m_itemDragOrigin->Y) < 5) return;
            m_suppressSurfaceTap = true;
            FileSurface().CapturePointer(args.Pointer());
            if (point.X < 0 || point.Y < 0 || point.X > FileSurface().ActualWidth() || point.Y > FileSurface().ActualHeight())
            {
                m_itemDragOrigin.reset();
                SetDropHighlight(nullptr);
                FileSurface().ReleasePointerCapture(args.Pointer());
                m_shellDragRequested = true;
                FileSurface().StartDragAsync(pt);
                return;
            }
            auto target = itemAt(point);
            if (target)
            {
                auto path = std::filesystem::path(unbox_value_or<hstring>(target.Tag(), L"").c_str());
                std::error_code err;
                if (!CanManage(path) || !std::filesystem::is_directory(path, err) ||
                    std::ranges::find(m_itemDragFiles, path) != m_itemDragFiles.end()) target = nullptr;
            }
            SetDropHighlight(target);
            args.Handled(true);
        })), true);
        surface.AddHandler(UIElement::PointerReleasedEvent(), box_value(PointerEventHandler([this, itemAt](auto const&, PointerRoutedEventArgs const& args)
        {
            if (!m_itemDragOrigin) return;
            auto pt = args.GetCurrentPoint(FileSurface()).Position();
            bool dragged = std::abs(pt.X - m_itemDragOrigin->X) >= 5 || std::abs(pt.Y - m_itemDragOrigin->Y) >= 5;
            m_itemDragOrigin.reset();
            auto target = dragged ? itemAt(pt) : nullptr;
            SetDropHighlight(nullptr);
            FileSurface().ReleasePointerCapture(args.Pointer());
            if (!target) return;
            auto folder = std::filesystem::path(unbox_value_or<hstring>(target.Tag(), L"").c_str());
            std::error_code err;
            if (CanManage(folder) && std::filesystem::is_directory(folder, err) &&
                std::ranges::find(m_itemDragFiles, folder) == m_itemDragFiles.end())
            {
                args.Handled(true);
                TransferDroppedFiles(m_itemDragFiles, folder, true);
            }
        })), true);
        surface.PointerCanceled([this](auto const&, auto const&) { m_itemDragOrigin.reset(); SetDropHighlight(nullptr); });
        surface.DragStarting([this](auto const&, DragStartingEventArgs const& args)
        {
            if (!m_shellDragRequested || m_itemDragFiles.empty()) return;
            args.AllowedOperations(Windows::ApplicationModel::DataTransfer::DataPackageOperation::Copy |
                Windows::ApplicationModel::DataTransfer::DataPackageOperation::Move);
            args.Cancel(!PrepareFileDrag(args.Data(), m_itemDragFiles));
        });
        surface.DropCompleted([this](auto const&, auto const&) { m_shellDragRequested = false; m_fileDragPaths.clear(); });
    }

    void MainWindow::SetupExplorer()
    {
        HomeView().PreviewKeyDown([this](auto const& sender, KeyRoutedEventArgs const& args)
        {
            if (Within(args.OriginalSource().try_as<DependencyObject>(), FileSurface())) Root_KeyDown(sender, args);
        });
        RootGrid().AddHandler(UIElement::PointerPressedEvent(), box_value(PointerEventHandler([this](auto const&, PointerRoutedEventArgs const& args)
        {
            auto source=args.OriginalSource().try_as<DependencyObject>();
            if (m_detailsOpen && !m_editing && !m_splitterDragging && m_store && !m_store->Settings().detailsPinned &&
                !Within(source,DetailsPane()) && !Within(source,DetailsRail()))
            { m_detailsOpen=false; UpdateNotebook(); }
            if(!Within(source,Notebook()) && !Within(source,NotesRail())) OutsideNotebook();
            if(!Within(source,TerminalPanel()) && !Within(source,TerminalRail())) CloseTerminalPanel();
            if (DefinitionPanel().Visibility() == Visibility::Visible &&
                !Within(args.OriginalSource().try_as<DependencyObject>(), DefinitionPanel())) CloseDefinitionPanel();
        })), true);
        HomeView().SizeChanged([this](auto const&, auto const&)
        {
            if(m_store) UpdateNotebook();

        });
        SetupItemMove();
        auto surface = FileSurface();
        surface.AddHandler(UIElement::PointerPressedEvent(), box_value(PointerEventHandler([this](auto const&, PointerRoutedEventArgs const& args)
        {
            m_suppressSurfaceTap = false;
            if (!m_ready || args.Pointer().PointerDeviceType() != Microsoft::UI::Input::PointerDeviceType::Mouse) return;
            auto point = args.GetCurrentPoint(SelectionLayer());
            if (!point.Properties().IsLeftButtonPressed()) return;
            auto list = ActiveFileList();
            for (auto node = args.OriginalSource().try_as<DependencyObject>(); node && node != FileSurface(); node = VisualTreeHelper::GetParent(node))
                if (node.try_as<ListViewItem>() || node.try_as<GridViewItem>() || node.try_as<ScrollBar>()) return;
            list.Focus(FocusState::Pointer);
            if (!FileSurface().CapturePointer(args.Pointer())) return;
            m_marqueeOrigin = point.Position();
            m_marqueeInitial = (GetKeyState(VK_CONTROL) & 0x8000) ? SelectedPaths() : std::vector<std::filesystem::path>{};
            args.Handled(true);
        })), true);
        surface.PointerMoved([this](auto const&, PointerRoutedEventArgs const& args)
        {
            if (!m_marqueeOrigin) return;
            auto point = args.GetCurrentPoint(SelectionLayer()).Position();
            auto start = *m_marqueeOrigin;
            if (!m_marqueeActive && std::abs(point.X-start.X) < 4 && std::abs(point.Y-start.Y) < 4) return;
            if (!m_marqueeActive && m_marqueeInitial.empty())
            {
                m_marqueeActive = true;
                ActiveFileList().SelectedItems().Clear();
            }
            m_marqueeActive = true;
            m_suppressSurfaceTap = true;
            float left = (std::min)(point.X, start.X), top = (std::min)(point.Y, start.Y);
            auto width = std::abs(point.X-start.X), height = std::abs(point.Y-start.Y);
            Canvas::SetLeft(SelectionBox(), left); Canvas::SetTop(SelectionBox(), top);
            SelectionBox().Width(width); SelectionBox().Height(height); SelectionBox().Visibility(Visibility::Visible);
            auto list = ActiveFileList();
            auto panel = list.ItemsPanelRoot();
            if (!panel) return;
            for (auto const& child : panel.Children())
            {
                auto box = child.try_as<FrameworkElement>();
                if (!box) continue;
                auto item = list.ItemFromContainer(box).try_as<FrameworkElement>();
                if (!item) continue;
                auto bounds = box.TransformToVisual(SelectionLayer()).TransformBounds({ 0, 0,
                    static_cast<float>(box.ActualWidth()), static_cast<float>(box.ActualHeight()) });
                bool hit = bounds.X < left+width && bounds.X+bounds.Width > left && bounds.Y < top+height && bounds.Y+bounds.Height > top;
                auto key = std::filesystem::path(unbox_value_or<hstring>(item.Tag(), L"").c_str());
                bool select = hit != (std::ranges::find(m_marqueeInitial, key) != m_marqueeInitial.end());
                uint32_t index{};
                bool wasSelected = list.SelectedItems().IndexOf(item, index);
                if (select && !wasSelected) list.SelectedItems().Append(item);
                if (!select && wasSelected) list.SelectedItems().RemoveAt(index);
            }
            args.Handled(true);
        });
        auto finish = [this](auto const&, PointerRoutedEventArgs const& args)
        {
            if (!m_marqueeOrigin) return;
            m_marqueeOrigin.reset();
            auto list = ActiveFileList();
            if (!m_marqueeActive && m_marqueeInitial.empty()) list.SelectedItems().Clear();
            m_marqueeActive = false;
            m_marqueeInitial.clear();
            SelectionBox().Visibility(Visibility::Collapsed);
            FileSurface().ReleasePointerCapture(args.Pointer());
            m_lastSelectedPath = SelectedOne();
            ShowSelectedMetadata(); UpdateAddress();
        };
        surface.PointerReleased(finish);
        surface.PointerCaptureLost(finish);
        surface.PointerCanceled(finish);
    }
}

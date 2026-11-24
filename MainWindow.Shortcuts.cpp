#include "pch.h"
#include "MainWindow.xaml.h"
#include <shellapi.h>
#include <shlwapi.h>
#include <algorithm>
#include <array>
#include <utility>
#include <winrt/Microsoft.UI.Xaml.Automation.h>

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Media;
using namespace winrt::Microsoft::UI::Xaml::Media::Imaging;

namespace
{
    std::wstring Glyph(std::wstring const& name)
    {
        if (name == L"Folder") return L"\xE8B7";
        if (name == L"Document") return L"\xE8A5";
        if (name == L"Terminal") return L"\xE756";
        if (name == L"Web") return L"\xE774";
        if (name == L"Code") return L"\xE943";
        if (name == L"Star") return L"\xE734";
        if (name == L"Music") return L"\xE8D6";
        if (name == L"Image") return L"\xEB9F";
        if (name == L"Tools") return L"\xE713";
        return L"\xE71D";
    }

    ImageSource ShortcutImage(std::filesystem::path path)
    {
        if (path.empty()) return nullptr;
        if (path.is_relative()) path = ::HudesNative::ExecutableFolder() / path;
        if (!std::filesystem::is_regular_file(path))
            path = ::HudesNative::ExecutableFolder() / L"logo" / path.filename();
        if (!std::filesystem::is_regular_file(path)) return nullptr;
        std::array<wchar_t, 32768> uriBuffer{};
        DWORD uriLength = static_cast<DWORD>(uriBuffer.size());
        check_hresult(UrlCreateFromPathW(path.c_str(), uriBuffer.data(), &uriLength, 0));
        Uri uri(uriBuffer.data());
        if (_wcsicmp(path.extension().c_str(), L".svg") == 0) return SvgImageSource(uri);
        return BitmapImage(uri);
    }

    std::wstring QuoteArgument(std::wstring const& value)
    {
        std::wstring result = L"\"";
        size_t slashes{};
        for (auto ch : value)
        {
            if (ch == L'\\') { ++slashes; continue; }
            if (ch == L'\"') { result.append(slashes * 2 + 1, L'\\'); result += ch; }
            else { result.append(slashes, L'\\'); result += ch; }
            slashes = 0;
        }
        result.append(slashes * 2, L'\\');
        return result + L'\"';
    }

    struct TargetFieldEvents
    {
        Button::Click_revoker browse;
        ComboBox::SelectionChanged_revoker type;
    };

    TargetFieldEvents BindTargetFields(ComboBox type, TextBox target, Button browse,
        std::function<std::optional<std::filesystem::path>()> chooseFile)
    {
        auto update = [target, browse](ComboBox const& selected)
        {
            bool file = selected.SelectedIndex() == 0;
            target.Header(box_value(file ? L"App, file or shortcut path" : L"Command"));
            browse.Visibility(file ? Visibility::Visible : Visibility::Collapsed);
        };
        update(type);
        return {
            browse.Click(auto_revoke, [target, type, chooseFile](auto const&, auto const&)
            {
                if (auto path = chooseFile()) { target.Text(path->wstring()); type.SelectedIndex(0); }
            }),
            type.SelectionChanged(auto_revoke, [update](IInspectable const& sender, auto const&) { update(sender.as<ComboBox>()); })
        };
    }

    void StartProcess(HWND owner, std::wstring const& file, std::wstring const& arguments = {})
    {
        SHELLEXECUTEINFOW info{ sizeof(info) };
        info.hwnd = owner;
        info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
        info.lpVerb = L"open";
        info.lpFile = file.c_str();
        info.lpParameters = arguments.empty() ? nullptr : arguments.c_str();
        info.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&info)) throw_last_error();
        if (info.hProcess) CloseHandle(info.hProcess);
    }

}

namespace HudesNative
{
    bool ShortcutFieldsReleased()
    {
        weak_ref<Button> button;
        weak_ref<TextBox> input;
        weak_ref<ComboBox> combo;
        {
            Button browse;
            TextBox target;
            ComboBox type;
            button = make_weak(browse);
            input = make_weak(target);
            combo = make_weak(type);
            auto bindings = BindTargetFields(type, target, browse, [] { return std::optional<std::filesystem::path>{}; });
        }
        return !button.get() && !input.get() && !combo.get();
    }
}

namespace winrt::HudesNative::implementation
{
    fire_and_forget MainWindow::UpdateRecords(int action, std::vector<std::filesystem::path> paths,
        std::wstring description, std::vector<std::wstring> tags, std::optional<std::wstring> group, std::wstring status)
    {
        if (paths.empty() || std::ranges::any_of(paths, [&](auto const& path) { return !CanManage(path); })) co_return;
        auto lifetime = get_strong();
        auto ui = apartment_context();
        auto ready = std::exchange(m_ready, false);
        MainInputContainer().IsEnabled(false);
        auto title = Title();
        Title(L"Hudes 1.2.9s · Updating items…");
        std::exception_ptr error;
        co_await resume_background();
        try
        {
            if (action == 0) m_store->SaveNote(paths.at(0), std::move(description), std::move(tags), std::move(group));
            else if (action == 1) m_store->SetGroup(paths, std::move(group));
            else m_store->SetStatus(paths, status);
        }
        catch (...) { error = std::current_exception(); }
        co_await ui;
        m_ready = ready; MainInputContainer().IsEnabled(true); Title(title);
        if (error) { ShowMessage(L"Update items", ::HudesNative::FriendlyError(error)); co_return; }
        SetMetadataEditing(false);
        if (action <= 1 && paths.size() == 1 && IsArchiveFolder(m_currentFolder))
        {
            auto note = m_store->Get(paths.front());
            if (note && note->archived && ArchiveLocation(*note) != m_currentFolder)
            {
                m_selectionToRestore = paths;
                NavigateTo(ArchiveLocation(*note));
            }
            else RefreshFiles();
        }
        else RefreshFiles();
        RefreshSearch(); ShowSelectedMetadata();
    }

    void MainWindow::LoadSearchFilters()
    {
        auto loading = std::exchange(m_loadingControls, true);
        auto restore = wil::scope_exit([&] { m_loadingControls = loading; });
        auto selectedGroup = SearchGroupFilter().SelectedItem().try_as<ComboBoxItem>();
        auto id = selectedGroup ? unbox_value_or<hstring>(selectedGroup.Tag(), L"") : hstring{};
        auto tag = ComboText(SearchTagFilter());
        SearchGroupFilter().Items().Clear();
        auto add = [&](std::wstring const& label, std::wstring const& key)
        {
            ComboBoxItem item; item.Content(box_value(label)); item.Tag(box_value(key));
            SearchGroupFilter().Items().Append(item);
            if (key == id.c_str()) SearchGroupFilter().SelectedItem(item);
        };
        add(L"All groups", L""); add(L"Ungrouped", L"__ungrouped");
        for (auto const& group : m_store->Groups()) add(group.name, group.id);
        if (SearchGroupFilter().SelectedIndex() < 0) SearchGroupFilter().SelectedIndex(0);
        SearchTagFilter().Items().Clear();
        ComboBoxItem all; all.Content(box_value(L"All tags")); SearchTagFilter().Items().Append(all);
        for (auto const& value : m_store->Tags()) { ComboBoxItem item; item.Content(box_value(value)); SearchTagFilter().Items().Append(item); }
        SetComboText(SearchTagFilter(), tag);
        FilterDefinitionCombo(SearchGroupFilter(), SearchGroupFind().Text());
        FilterDefinitionCombo(SearchTagFilter(), SearchTagFind().Text());
    }

    void MainWindow::SearchFilter_Changed(IInspectable const&, SelectionChangedEventArgs const&)
    {
        if (m_store && m_ready && !m_loadingControls) RefreshSearch(false);
    }

    void MainWindow::ClearSearchFilters_Click(IInspectable const&, RoutedEventArgs const&)
    {
        m_loadingControls = true;
        SearchGroupFind().Text(L"");
        SearchTagFind().Text(L"");
        SearchGroupFilter().SelectedIndex(0); SearchTagFilter().SelectedIndex(0); SearchStatusFilter().SelectedIndex(0);
        m_loadingControls = false;
        RefreshSearch(false);
    }

    void MainWindow::RenderTagChips(std::vector<std::wstring> const& tags)
    {
        TagChipsPanel().Children().Clear();
        TagPreviewPanel().Visibility(tags.empty() ? Visibility::Collapsed : Visibility::Visible);
        auto const& settings = m_store->Settings();
        for (auto const& tag : tags)
        {
            auto matches = [tag](auto const& filter) { return _wcsicmp(filter.c_str(), tag.c_str()) == 0; };
            auto active = std::ranges::any_of(m_tagFilters, matches);
            Primitives::ToggleButton chip;
            chip.Content(box_value(L"#" + tag));
            chip.HorizontalAlignment(HorizontalAlignment::Left);
            chip.IsChecked(active);
            chip.FontFamily(FontFamily(settings.Look().fonts[::HudesNative::TagFont].family)); chip.FontSize(settings.Look().fonts[::HudesNative::TagFont].size);
            chip.Foreground(active ? UiBrush(L"InkBrush") : FontBrush(::HudesNative::TagFont));
            chip.Background(active ? UiBrush(L"SelectedBrush") : UiBrush(L"ControlBrush"));
            chip.Padding({8,4,8,4});
            ToolTipService::SetToolTip(chip, box_value((active ? L"Remove tag filter: " : L"Filter files by ") + tag));
            chip.Click([weak = get_weak(), tag, matches](auto const&, auto const&)
            {
                if (auto self = weak.get())
                {
                    if (std::erase_if(self->m_tagFilters, matches) == 0) self->m_tagFilters = { tag };
                    self->UpdateTagFilterButton();
                    self->ShowTab(self->HomeView(), self->HomeTabButton());
                    self->RefreshFiles();
                }
            });
            TagChipsPanel().Children().Append(chip);
        }
    }

    void MainWindow::LoadShortcuts()
    {
        auto const& shortcuts = m_store->Settings().shortcuts;
        auto const perSide = shortcuts.size() / 2;
        for (auto const& row : { LeftShortcuts(), RightShortcuts() })
        {
            row.Children().Clear();
            row.ColumnDefinitions().Clear();
            for (size_t column = 0; column < perSide; ++column)
            {
                ColumnDefinition definition;
                definition.Width({ 1, GridUnitType::Star });
                row.ColumnDefinitions().Append(definition);
            }
        }
        ShortcutSettingsPanel().Children().Clear();
        for (size_t index = 0; index < shortcuts.size(); ++index)
        {
            auto const& shortcut = shortcuts[index];
            Button button;
            button.HorizontalAlignment(HorizontalAlignment::Stretch);
            button.VerticalAlignment(VerticalAlignment::Stretch);
            button.HorizontalContentAlignment(HorizontalAlignment::Stretch);
            button.VerticalContentAlignment(VerticalAlignment::Stretch);
            button.MinWidth(0);
            button.Padding({ 3 });
            button.Background(UiBrush(L"ControlBrush"));
            ScaleTransform scale;
            scale.ScaleX(0.8);
            scale.ScaleY(0.8);
            button.RenderTransformOrigin({ 0.5f, 0.5f });
            button.RenderTransform(scale);
            Viewbox graphic;
            graphic.Stretch(Stretch::Uniform);
            graphic.StretchDirection(StretchDirection::DownOnly);
            FontIcon icon;
            icon.FontFamily(FontFamily(L"Segoe Fluent Icons"));
            icon.Glyph(shortcut.target.empty() ? L"\xE710" : Glyph(shortcut.icon));
            icon.FontSize(28 * 0.85);
            icon.Foreground(UiBrush(L"AccentBrush"));
            graphic.Child(icon);
            button.Content(graphic);
            if (!shortcut.iconPath.empty())
            {
                try
                {
                    if (auto source = ShortcutImage(shortcut.iconPath))
                    {
                        Image image; image.Width(34 * 0.85); image.Height(34 * 0.85); image.Stretch(Stretch::Uniform);
                        image.Source(source);
                        graphic.Child(image);
                    }
                }
                catch (...) { /* Retain the built-in icon if an image cannot load. */ }
            }
            auto title = shortcut.label.empty() ? L"Shortcut " + std::to_wstring(index + 1) : shortcut.label;
            Automation::AutomationProperties::SetName(button, shortcut.target.empty() ? L"Configure " + title : title);
            ToolTipService::SetToolTip(button, box_value(title + (shortcut.target.empty() ? L" · click to configure" : L" · " + shortcut.type + L"\n" + shortcut.target)));
            button.Click([weak = get_weak(), index](auto const&, auto const&) { if (auto self = weak.get()) self->LaunchShortcut(index); });
            MenuFlyout menu; MenuFlyoutItem edit; edit.Text(L"Configure shortcut"); edit.Click([weak = get_weak(), index](auto const&, auto const&) { if (auto self = weak.get()) self->EditShortcut(index); }); menu.Items().Append(edit); button.ContextFlyout(menu);
            Grid::SetColumn(button, static_cast<int32_t>(index % perSide));
            (index < perSide ? LeftShortcuts() : RightShortcuts()).Children().Append(button);
            Button settings; settings.HorizontalAlignment(HorizontalAlignment::Stretch); settings.HorizontalContentAlignment(HorizontalAlignment::Left);
            settings.Content(box_value(std::to_wstring(index + 1) + L"   " + title + L"   ·   " + (shortcut.target.empty() ? L"Not assigned" : shortcut.type)));
            settings.Click([weak = get_weak(), index](auto const&, auto const&) { if (auto self = weak.get()) self->EditShortcut(index); });
            ShortcutSettingsPanel().Children().Append(settings);
        }
    }

    fire_and_forget MainWindow::EditShortcut(size_t index)
    {
        auto lifetime = get_strong();
        try
        {
            auto shortcut = m_store->Settings().shortcuts.at(index);
            ContentDialog dialog; dialog.XamlRoot(RootGrid().XamlRoot()); dialog.Title(box_value(L"Shortcut " + std::to_wstring(index + 1)));
            dialog.PrimaryButtonText(L"Save shortcut"); dialog.SecondaryButtonText(L"Clear slot"); dialog.CloseButtonText(L"Cancel");
            StackPanel content;
            content.Spacing(12);
            content.MinWidth(410);
            Grid identity;
            identity.ColumnSpacing(10);
            ColumnDefinition nameColumn;
            nameColumn.Width({ 1, GridUnitType::Star });
            identity.ColumnDefinitions().Append(nameColumn);
            ColumnDefinition typeColumn;
            typeColumn.Width({ 115, GridUnitType::Pixel });
            identity.ColumnDefinitions().Append(typeColumn);
            TextBox label;
            label.Header(box_value(L"Name"));
            label.Text(shortcut.label);
            identity.Children().Append(label);
            ComboBox type;
            type.Header(box_value(L"Launch type"));
            type.HorizontalAlignment(HorizontalAlignment::Stretch);
            for (auto const* value : { L"File", L"CMD", L"Bash" })
            {
                ComboBoxItem item;
                item.Content(box_value(value));
                type.Items().Append(item);
            }
            SetComboText(type, shortcut.type);
            Grid::SetColumn(type, 1);
            identity.Children().Append(type);
            content.Children().Append(identity);
            TextBox target;
            target.Text(shortcut.target);
            target.AcceptsReturn(true);
            target.TextWrapping(TextWrapping::Wrap);
            target.MinHeight(58);
            content.Children().Append(target);
            Button browse;
            browse.Content(box_value(L"Choose file…"));
            auto targetEvents = BindTargetFields(type, target, browse, [this]
            {
                return ::HudesNative::PickOpenFile(WindowHandle(), L"Choose app, file, or shortcut", L"All files", L"*.*");
            });
            content.Children().Append(browse);
            dialog.IsPrimaryButtonEnabled(!target.Text().empty());
            target.TextChanged([weakDialog = make_weak(dialog)](IInspectable const& sender, auto const&)
            {
                if (auto owner = weakDialog.get()) owner.IsPrimaryButtonEnabled(!sender.as<TextBox>().Text().empty());
            });

            TextBlock heading;
            heading.Text(L"Built-in logos");
            heading.FontSize(13);
            heading.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            content.Children().Append(heading);
            GridView icons;
            icons.SelectionMode(ListViewSelectionMode::Single);
            icons.IsSwipeEnabled(false);
            icons.MaxWidth(430);
            icons.HorizontalAlignment(HorizontalAlignment::Left);
            for (auto const* value : { L"App", L"Folder", L"Document", L"Terminal", L"Web", L"Code", L"Star", L"Music", L"Image", L"Tools" })
            {
                GridViewItem choice;
                choice.Width(74);
                choice.Height(55);
                choice.Margin({ 2 });
                choice.Tag(box_value(value));
                Automation::AutomationProperties::SetName(choice, value);
                StackPanel tile;
                tile.Spacing(4);
                FontIcon mark;
                mark.FontFamily(FontFamily(L"Segoe Fluent Icons"));
                mark.Glyph(Glyph(value));
                mark.FontSize(19);
                mark.Foreground(UiBrush(L"AccentBrush"));
                tile.Children().Append(mark);
                TextBlock title;
                title.Text(value);
                title.FontSize(11);
                title.HorizontalAlignment(HorizontalAlignment::Center);
                tile.Children().Append(title);
                choice.Content(tile);
                icons.Items().Append(choice);
                if (shortcut.icon == value) icons.SelectedItem(choice);
            }
            if (icons.SelectedIndex() < 0) icons.SelectedIndex(0);
            content.Children().Append(icons);

            auto customPath = std::make_shared<std::wstring>(shortcut.iconPath);
            Image preview;
            preview.Width(28); preview.Height(28);
            preview.Stretch(Stretch::Uniform);
            TextBlock customName;
            customName.TextTrimming(TextTrimming::CharacterEllipsis);
            customName.VerticalAlignment(VerticalAlignment::Center);
            customName.MaxWidth(355);
            auto previewRow = StackPanel();
            previewRow.Orientation(Orientation::Horizontal);
            previewRow.Spacing(8);
            previewRow.Children().Append(preview);
            previewRow.Children().Append(customName);
            content.Children().Append(previewRow);
            auto updatePreview = [preview, customName, customPath]
            {
                preview.Source(nullptr);
                try { preview.Source(ShortcutImage(*customPath)); }
                catch (...) { /* The filename remains visible if the preview cannot load. */ }
                preview.Visibility(preview.Source() ? Visibility::Visible : Visibility::Collapsed);
                customName.Text(customPath->empty() ? L"Using a built-in logo" : std::filesystem::path(*customPath).filename().wstring());
            };
            updatePreview();
            StackPanel imageActions;
            imageActions.Orientation(Orientation::Horizontal);
            imageActions.Spacing(8);
            Button choose;
            choose.Content(box_value(L"Choose custom logo…"));
            choose.Click([this, customPath, updatePreview](auto const&, auto const&)
            {
                if (auto path = ::HudesNative::PickOpenFile(WindowHandle(), L"Choose logo", L"Images", L"*.png;*.jpg;*.jpeg;*.svg;*.ico;*.bmp"))
                {
                    *customPath = path->wstring();
                    updatePreview();
                }
            });
            imageActions.Children().Append(choose);
            Button clear;
            clear.Content(box_value(L"Use built-in"));
            clear.Click([customPath, updatePreview](auto const&, auto const&)
            {
                customPath->clear();
                updatePreview();
            });
            imageActions.Children().Append(clear);
            icons.SelectionChanged([customPath, updatePreview](auto const&, auto const&)
            {
                customPath->clear();
                updatePreview();
            });
            content.Children().Append(imageActions);
            ScrollViewer scroll; scroll.MaxHeight(560); scroll.Content(content); dialog.Content(scroll);
            auto dirty=[&,shortcut]
            {
                auto selected=icons.SelectedItem().try_as<GridViewItem>();
                auto icon=selected?unbox_value_or<hstring>(selected.Tag(),L"App"):hstring(L"App");
                return label.Text()!=shortcut.label || ComboText(type)!=shortcut.type || target.Text()!=shortcut.target ||
                    icon!=shortcut.icon || *customPath!=shortcut.iconPath;
            };
            auto anchor=(index<6?LeftShortcuts():RightShortcuts()).Children().GetAt(static_cast<uint32_t>(index%6)).as<FrameworkElement>();
            if(SettingsView().Visibility()==Visibility::Visible) anchor=ShortcutSettingsPanel().Children().GetAt(static_cast<uint32_t>(index)).as<FrameworkElement>();
            auto result = co_await ShowLightEditor(dialog,anchor,dirty,[]{return true;});
            if (result == ContentDialogResult::None) co_return;
            if (result == ContentDialogResult::Secondary) shortcut = {};
            else
            {
                shortcut.label = label.Text().c_str();
                shortcut.type = ComboText(type);
                shortcut.target = target.Text().c_str();
                auto selectedIcon = icons.SelectedItem().try_as<GridViewItem>();
                shortcut.icon = selectedIcon ? std::wstring(unbox_value_or<hstring>(selectedIcon.Tag(), L"App")) : L"App";
                shortcut.iconPath = *customPath;
                if (shortcut.target.empty()) { ShowMessage(L"Shortcut", L"Choose a file or enter a command. Use Clear slot to remove this shortcut."); co_return; }
                if (!shortcut.iconPath.empty())
                {
                    shortcut.iconPath = ::HudesNative::ImportShortcutLogo(::HudesNative::ExecutableFolder(), shortcut.iconPath).generic_wstring();
                }
            }
            m_store->Settings().shortcuts.at(index) = std::move(shortcut); m_store->Save(); LoadShortcuts();
        }
        catch (...) { ShowMessage(L"Shortcut", ::HudesNative::FriendlyError()); }
    }

    void MainWindow::LaunchShortcut(size_t index)
    {
        try
        {
            auto const shortcut = m_store->Settings().shortcuts.at(index);
            if (shortcut.target.empty()) { EditShortcut(index); return; }
            if (shortcut.type == L"File") StartProcess(WindowHandle(), shortcut.target);
            else if (shortcut.type == L"CMD")
            {
                std::array<wchar_t, MAX_PATH> system{}; GetSystemDirectoryW(system.data(), static_cast<UINT>(system.size()));
                StartProcess(WindowHandle(), (std::filesystem::path(system.data()) / L"cmd.exe").wstring(), L"/d /k " + shortcut.target);
            }
            else if (shortcut.type == L"Bash")
            {
                auto executable = m_store->Settings().bashExecutable;
                if (executable.empty())
                {
                    std::array<wchar_t, 32768> buffer{};
                    auto length = GetEnvironmentVariableW(L"ProgramFiles", buffer.data(), static_cast<DWORD>(buffer.size()));
                    auto git = std::filesystem::path(std::wstring(buffer.data(), length)) / L"Git" / L"bin" / L"bash.exe";
                    if (std::filesystem::is_regular_file(git)) executable = git.wstring();
                    else
                    {
                        auto found = SearchPathW(nullptr, L"bash.exe", nullptr, static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
                        if (!found || found >= buffer.size()) throw hresult_error(E_FAIL, L"Install Bash or set its executable path in Settings.");
                        executable.assign(buffer.data(), found);
                    }
                }
                StartProcess(WindowHandle(), executable, L"-lc " + QuoteArgument(shortcut.target));
            }
            else throw hresult_invalid_argument(L"Unknown shortcut type. Configure this shortcut again.");
        }
        catch (...) { ShowMessage(L"Launch shortcut", ::HudesNative::FriendlyError()); }
    }
}

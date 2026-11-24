#include "pch.h"
#include <winrt/Microsoft.UI.Input.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include "MainWindow.xaml.h"
#include "Workspace.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#include <dwrite.h>
#include <microsoft.ui.xaml.window.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <iomanip>
#include <sstream>
#include <utility>
#include <cmath>
#include <shellapi.h>
#include <userconsentverifierinterop.h>

#pragma comment(lib, "dwrite.lib")

using namespace winrt;
using namespace winrt::Windows::ApplicationModel::DataTransfer;
using namespace winrt::Windows::Security::Credentials::UI;
using namespace winrt::Windows::Storage;
using namespace winrt::Windows::Storage::FileProperties;
using namespace winrt::Microsoft::UI;
using namespace winrt::Microsoft::UI::Windowing;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Controls::Primitives;
using namespace winrt::Microsoft::UI::Xaml::Input;
using namespace winrt::Microsoft::UI::Xaml::Media;
using namespace winrt::Microsoft::UI::Xaml::Media::Imaging;

namespace
{
    constexpr wchar_t const* Transparent = L"Transparent";

    std::wstring Lower(std::wstring value)
    {
        std::ranges::transform(value, value.begin(), [](wchar_t character) { return static_cast<wchar_t>(::towlower(character)); });
        return value;
    }

    std::wstring Upper(std::wstring value)
    {
        std::ranges::transform(value, value.begin(), [](wchar_t character) { return static_cast<wchar_t>(::towupper(character)); });
        return value;
    }

    bool EqualText(std::wstring const& left, std::wstring const& right)
    {
        return _wcsicmp(left.c_str(), right.c_str()) == 0;
    }

    winrt::Windows::UI::Color ParseColor(std::wstring value, std::uint8_t alpha = 255)
    {
        if (value.starts_with(L'#')) value.erase(value.begin());
        unsigned color{};
        if (value.size() != 6 || swscanf_s(value.c_str(), L"%x", &color) != 1) color = 0xDDE7FF;
        return winrt::Windows::UI::Color{ alpha, static_cast<std::uint8_t>((color >> 16) & 0xFF),
            static_cast<std::uint8_t>((color >> 8) & 0xFF), static_cast<std::uint8_t>(color & 0xFF) };
    }

    SolidColorBrush ColorBrush(std::wstring value, std::uint8_t alpha = 255)
    {
        return SolidColorBrush(ParseColor(std::move(value), alpha));
    }

    TextBlock MakeText(std::wstring const& value, double size = 11, winrt::Windows::UI::Text::FontWeight weight = {})
    {
        TextBlock text;
        text.Text(value);
        text.FontSize(size);
        if (weight.Weight) text.FontWeight(weight);
        text.TextTrimming(TextTrimming::CharacterEllipsis);
        return text;
    }

    std::wstring StatusGlyph(std::wstring const& status)
    {
        if (EqualText(status, L"Done")) return L"✓";
        if (EqualText(status, L"Ongoing") || EqualText(status, L"Important")) return L"✎";
        return L"!";
    }

    wchar_t const* StatusColorKey(std::wstring const& status)
    {
        if (EqualText(status, L"Done")) return L"DoneBrush";
        if (EqualText(status, L"Ongoing")) return L"OngoingBrush";
        if (EqualText(status, L"Important")) return L"ImportantBrush";
        return EqualText(status,L"Emergency") ? L"UrgentBrush" : L"NewStatusBrush";
    }

    std::wstring Sanitize(std::wstring value)
    {
        constexpr wchar_t invalid[] = L"<>:\"/\\|?*";
        for (auto& character : value) if (wcschr(invalid, character)) character = L'_';
        return value;
    }

    std::vector<std::wstring> SystemFonts()
    {
        std::vector<std::wstring> result{ L"Dubai", L"Segoe UI Variable", L"Segoe UI", L"Arial", L"Calibri", L"Tahoma", L"Times New Roman" };
        com_ptr<IDWriteFactory> factory;
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(factory.put_void())))) return result;
        com_ptr<IDWriteFontCollection> collection;
        if (FAILED(factory->GetSystemFontCollection(collection.put(), FALSE))) return result;
        for (UINT32 index = 0; index < collection->GetFontFamilyCount(); ++index)
        {
            com_ptr<IDWriteFontFamily> family;
            if (FAILED(collection->GetFontFamily(index, family.put()))) continue;
            com_ptr<IDWriteLocalizedStrings> names;
            if (FAILED(family->GetFamilyNames(names.put()))) continue;
            UINT32 selected{};
            BOOL found{};
            names->FindLocaleName(L"en-us", &selected, &found);
            if (!found) selected = 0;
            UINT32 length{};
            if (FAILED(names->GetStringLength(selected, &length))) continue;
            std::wstring name(length + 1, L'\0');
            if (FAILED(names->GetString(selected, name.data(), static_cast<UINT32>(name.size())))) continue;
            name.resize(length);
            if (!std::ranges::contains(result, name)) result.push_back(std::move(name));
        }
        std::ranges::sort(result, [](auto const& left, auto const& right) { return _wcsicmp(left.c_str(), right.c_str()) < 0; });
        return result;
    }

    std::wstring ColorHex(winrt::Windows::UI::Color const& color)
    {
        wchar_t value[8]{};
        swprintf_s(value, L"#%02X%02X%02X", color.R, color.G, color.B);
        return value;
    }

    void ApplyFontTree(DependencyObject const& root, FontFamily const& family, double size)
    {
        if (auto button=root.try_as<Button>(); button && button.Name()==L"DonateButton") return;
        if (auto control = root.try_as<Control>())
        {
            if (auto current=control.FontFamily(); current && (current.Source()==L"Segoe Fluent Icons" || current.Source()==L"Segoe MDL2 Assets")) return;
            control.FontFamily(family);
            control.FontSize(size);
        }
        if (auto text = root.try_as<TextBlock>())
        {
            text.FontFamily(family);
            text.FontSize(size);
        }
        auto const count = VisualTreeHelper::GetChildrenCount(root);
        for (int index = 0; index < count; ++index)
            ApplyFontTree(VisualTreeHelper::GetChild(root, index), family, size);
    }

    void ApplyColorTree(DependencyObject const& root, Brush const& brush)
    {
        if (auto button=root.try_as<Button>(); button && button.Name()==L"DonateButton") return;
        if (auto control = root.try_as<Control>()) control.Foreground(brush);
        if (auto text = root.try_as<TextBlock>()) text.Foreground(brush);
        auto const count = VisualTreeHelper::GetChildrenCount(root);
        for (int index = 0; index < count; ++index)
            ApplyColorTree(VisualTreeHelper::GetChild(root, index), brush);
    }

    constexpr wchar_t FileDragFormat[] = L"Hudes.WorkspaceItems";

    std::optional<std::filesystem::path> TaggedPathFromSource(DependencyObject source)
    {
        for (auto current = source; current; current = VisualTreeHelper::GetParent(current))
        {
            if (auto element = current.try_as<FrameworkElement>())
            {
                auto value = unbox_value_or<hstring>(element.Tag(), L"");
                if (!value.empty()) return std::filesystem::path(value.c_str());
            }
        }
        return std::nullopt;
    }
}

namespace winrt::HudesNative::implementation
{
    MainWindow::MainWindow()
    {
        InitializeComponent();
        ConfigureWindow();
        MainInputContainer().Visibility(Visibility::Collapsed);
        CompactWindow(true);
        SetupExplorer();
        SetupNotebook();
        for (auto grip : {DetailsGrip(), NotesGrip(), TerminalGrip()})
            grip.as<IUIElementProtected>().ProtectedCursor(Microsoft::UI::Input::InputSystemCursor::Create(
                grip==TerminalGrip() ? Microsoft::UI::Input::InputSystemCursorShape::SizeNorthSouth : Microsoft::UI::Input::InputSystemCursorShape::SizeWestEast));
        SetupTerminal();
        MainInputContainer().IsEnabled(false);
        Closed([this](auto const&, auto const&)
        {
            m_windowClosed = true;
            ++m_generation;
            m_notesTimer.Stop();
            m_detailsTimer.Stop();
            EndTerminalSession();
            m_watcher.Stop();
            if (m_terminalWeb) m_terminalWeb.Close();
        });
        RootGrid().Loaded([weak = get_weak()](auto const&, auto const&)
        {
            if (auto self = weak.get()) self->InitializeDatabase();
        });
    }

    MainWindow::~MainWindow()
    {
        m_watcher.Stop();
        ++m_generation;
        decltype(m_storageItems) stale;
        stale.swap(m_storageItems);
        m_items.clear();
    }

    Brush MainWindow::UiBrush(wchar_t const* key) const
    {
        return ColorBrush(m_store->Settings().Look().Color(key));
    }

    Brush MainWindow::FontBrush(::HudesNative::FontRole role) const
    {
        return ColorBrush(m_store->Settings().Look().fonts[role].color);
    }

    HWND MainWindow::WindowHandle() const
    {
        HWND handle{};
        auto native = this->try_as<::IWindowNative>();
        check_hresult(native->get_WindowHandle(&handle));
        return handle;
    }

    void MainWindow::ConfigureWindow()
    {
        auto handle = WindowHandle();
        auto id = winrt::Microsoft::UI::GetWindowIdFromWindow(handle);
        auto window = AppWindow::GetFromWindowId(id);
        auto icon = (::HudesNative::ExecutableFolder() / L"Assets" / L"hudes.ico").wstring();
        if (std::filesystem::exists(icon)) window.SetIcon(icon);
        if (auto area = DisplayArea::GetFromWindowId(id, DisplayAreaFallback::Primary))
        {
            auto work = area.WorkArea();
            auto scale = GetDpiForWindow(handle) / 96.0;
            auto width = (std::min)(static_cast<int>(1220 * scale), work.Width);
            auto height = (std::min)(static_cast<int>(790 * scale), work.Height);
            window.Resize({ width, height });
            window.Move({ work.X + (work.Width - width) / 2, work.Y + (work.Height - height) / 2 });
        }
    }

    void MainWindow::ApplyTheme()
    {
        auto dark = EqualText(m_store->Settings().themeMode, L"Dark");
        auto dictionary=RootGrid().Resources();
        auto const& look=m_store->Settings().Look();
        auto set=[&](wchar_t const* key,std::wstring const& hex)
        {
            auto brush=dictionary.Lookup(box_value(hstring(key))).as<SolidColorBrush>();
            auto color=ParseColor(hex);
            if (brush.Color()!=color) brush.Color(color);
        };
        for (size_t i=0;i<look.colors.size();++i) set(::HudesNative::paletteRoles[i].key,look.colors[i]);
        set(L"InkBrush",look.fonts[::HudesNative::AppFont].color);
        for (auto key : {L"TextControlForeground",L"TextControlForegroundFocused",L"ComboBoxForeground",L"CalendarViewDayItemForeground"})
            set(key,look.fonts[::HudesNative::AppFont].color);
        for (auto key : {L"TextControlBackground",L"ComboBoxBackground"}) set(key,look.Color(L"FieldBrush"));
        for (auto key : {L"TextControlBackgroundPointerOver",L"ComboBoxBackgroundPointerOver"}) set(key,look.Color(L"HoverBrush"));
        for (auto key : {L"TextControlBackgroundFocused",L"ComboBoxBackgroundPressed"}) set(key,look.Color(L"FocusBrush"));
        for (auto key : {L"TextControlBorderBrush"}) set(key,look.Color(L"FieldBorderBrush"));
        for (auto key : {L"FlyoutBackground",L"MenuFlyoutPresenterBackground",L"ComboBoxDropDownBackground",L"CalendarViewBackground"}) set(key,look.Color(L"PanelBrush"));
        set(L"TextControlPlaceholderForeground",look.Color(L"MutedBrush"));
        for (auto key : {L"TextControlBorderBrushFocused",L"ComboBoxBorderBrushFocused",L"FocusStrokeColorOuterBrush",L"AccentFillColorDefaultBrush"})
            set(key,look.Color(L"FocusLineBrush"));
        set(L"ToggleButtonBackgroundChecked",look.Color(L"SelectedBrush"));
        set(L"CheckBoxCheckBackgroundFillChecked",look.Color(L"FocusLineBrush"));
        set(L"ButtonBackgroundPointerOver",look.Color(L"HoverBrush"));
        set(L"ButtonBorderBrushPointerOver",look.Color(L"FieldBorderBrush"));
        set(L"ListViewItemBackgroundSelected",look.Color(L"SelectedBrush"));
        set(L"ListViewItemSelectionIndicatorBrush",look.Color(L"FocusLineBrush"));
        RootGrid().Background(dictionary.Lookup(box_value(L"SurfaceBrush")).as<SolidColorBrush>());
        RootGrid().RequestedTheme(dark ? ElementTheme::Dark : ElementTheme::Light);
        BOOL enabled = dark;
        DwmSetWindowAttribute(WindowHandle(), DWMWA_USE_IMMERSIVE_DARK_MODE, &enabled, sizeof(enabled));
        auto window = AppWindow::GetFromWindowId(winrt::Microsoft::UI::GetWindowIdFromWindow(WindowHandle()));
        if (window && window.TitleBar())
        {
            auto color=ParseColor(m_store->Settings().Look().Color(L"SurfaceBrush"));
            window.TitleBar().BackgroundColor(color);
            window.TitleBar().ButtonBackgroundColor(color);
            window.TitleBar().ForegroundColor(ParseColor(m_store->Settings().Look().fonts[::HudesNative::AppFont].color));
        }
    }

    void MainWindow::ApplyTypography()
    {
        auto const& settings = m_store->Settings();
        ApplyFontTree(RootGrid(), FontFamily(settings.Look().fonts[::HudesNative::AppFont].family), settings.Look().fonts[::HudesNative::AppFont].size);
        DesignCredit().FontSize(10);
        ApplyFontTree(FileGrid(), FontFamily(settings.Look().fonts[::HudesNative::FileFont].family), settings.Look().fonts[::HudesNative::FileFont].size);
        ApplyFontTree(FileDetails(), FontFamily(settings.Look().fonts[::HudesNative::FileFont].family), settings.Look().fonts[::HudesNative::FileFont].size);
        ApplyFontTree(GroupPanel(), FontFamily(settings.Look().fonts[::HudesNative::DescriptionFont].family), settings.Look().fonts[::HudesNative::DescriptionFont].size);
        ApplyFontTree(MetadataPanel(), FontFamily(settings.Look().fonts[::HudesNative::DescriptionFont].family), settings.Look().fonts[::HudesNative::DescriptionFont].size);
        NotePreviewText().FontFamily(FontFamily(settings.Look().fonts[::HudesNative::PreviewFont].family));
        NotePreviewText().FontSize(settings.Look().fonts[::HudesNative::PreviewFont].size);
        NotePreviewText().FontWeight(winrt::Windows::UI::Text::FontWeights::Bold());
        ApplyFontTree(SearchView(), FontFamily(settings.Look().fonts[::HudesNative::SearchFont].family), settings.Look().fonts[::HudesNative::SearchFont].size);
        NotePreviewTagsText().FontFamily(FontFamily(settings.Look().fonts[::HudesNative::TagFont].family));
        NotePreviewTagsText().FontSize(settings.Look().fonts[::HudesNative::TagFont].size);
        NotePreviewTagsText().FontWeight(winrt::Windows::UI::Text::FontWeights::Bold());
        ApplyColorTree(RootGrid(), ColorBrush(settings.Look().fonts[::HudesNative::AppFont].color));
        ApplyColorTree(FileGrid(), ColorBrush(settings.Look().fonts[::HudesNative::FileFont].color));
        ApplyColorTree(FileDetails(), ColorBrush(settings.Look().fonts[::HudesNative::FileFont].color));
        ApplyColorTree(GroupPanel(), ColorBrush(settings.Look().fonts[::HudesNative::DescriptionFont].color));
        ApplyColorTree(MetadataPanel(), ColorBrush(settings.Look().fonts[::HudesNative::DescriptionFont].color));
        ApplyColorTree(SearchView(), ColorBrush(settings.Look().fonts[::HudesNative::SearchFont].color));
        NotePreviewText().Foreground(ColorBrush(settings.Look().fonts[::HudesNative::PreviewFont].color));
        NotePreviewTagsText().Foreground(ColorBrush(settings.Look().fonts[::HudesNative::TagFont].color));
        RenderNoteCards();
        if (auto path = SelectedOne(); path && CanManage(*path)) { auto note = m_store->Get(*path); RenderTagChips(note ? note->tags : std::vector<std::wstring>{}); }
        LoadShortcuts();
    }

    void MainWindow::ApplyTextDirection()
    {
        auto rtl = m_store->Settings().metadataRtl;
        SideNoteBox().FlowDirection(rtl ? FlowDirection::RightToLeft : FlowDirection::LeftToRight);
        // WinUI mirrors leading alignment with the flow direction.
        SideNoteBox().TextAlignment(TextAlignment::Left);
        SideNoteBox().TextReadingOrder(TextReadingOrder::UseFlowDirection);
        SideNoteBox().HorizontalContentAlignment(HorizontalAlignment::Stretch);
        SideNoteBox().Language(rtl ? L"fa-IR" : L"en-US");
        NoteGroupCombo().FlowDirection(rtl ? FlowDirection::RightToLeft : FlowDirection::LeftToRight);
        NoteGroupCombo().Language(rtl ? L"fa-IR" : L"en-US");
        TagPickerButton().FlowDirection(rtl ? FlowDirection::RightToLeft : FlowDirection::LeftToRight);
        TagPickerButton().HorizontalContentAlignment(rtl ? HorizontalAlignment::Right : HorizontalAlignment::Left);
        TagPickerButton().Language(rtl ? L"fa-IR" : L"en-US");
        NotePreviewText().FlowDirection(rtl ? FlowDirection::RightToLeft : FlowDirection::LeftToRight);
        NotePreviewText().TextAlignment(TextAlignment::Left);
        NotePreviewText().HorizontalAlignment(HorizontalAlignment::Stretch);
        NotePreviewText().TextReadingOrder(TextReadingOrder::UseFlowDirection);
        NotePreviewText().Language(rtl ? L"fa-IR" : L"en-US");
        NotePreviewTagsText().FlowDirection(rtl ? FlowDirection::RightToLeft : FlowDirection::LeftToRight);
        NotePreviewTagsText().TextAlignment(TextAlignment::Left);
        NotePreviewTagsText().TextReadingOrder(TextReadingOrder::UseFlowDirection);
        NotePreviewTagsText().Language(rtl ? L"fa-IR" : L"en-US");
        DirectionButton().Content(box_value(rtl ? L"RTL" : L"LTR"));
        for (auto const& child : TagPickerPanel().Children())
        {
            if (auto check = child.try_as<CheckBox>())
            {
                check.FlowDirection(rtl ? FlowDirection::RightToLeft : FlowDirection::LeftToRight);
                check.Language(rtl ? L"fa-IR" : L"en-US");
            }
        }
    }

    void MainWindow::SetComboText(ComboBox const& combo, std::wstring const& value)
    {
        for (auto const& raw : combo.Items())
        {
            if (auto item = raw.try_as<ComboBoxItem>())
            {
                auto content = unbox_value_or<hstring>(item.Content(), L"");
                if (EqualText(content.c_str(), value)) { combo.SelectedItem(item); return; }
            }
        }
        if (combo.Items().Size()) combo.SelectedIndex(0);
    }

    std::wstring MainWindow::ComboText(ComboBox const& combo)
    {
        if (auto item = combo.SelectedItem().try_as<ComboBoxItem>()) return unbox_value_or<hstring>(item.Content(), L"").c_str();
        return {};
    }

    void MainWindow::LoadSettingsControls()
    {
        auto previousLoading = std::exchange(m_loadingControls, true);
        auto restoreLoading = wil::scope_exit([&] { m_loadingControls = previousLoading; });
        auto const& settings = m_store->Settings();
        DesktopFolderBox().Text(settings.desktopFolder.wstring());
        DefaultFolderBox().Text(settings.defaultFolder.wstring());
        ReminderHoursBox().Text(to_hstring(settings.reminderWarningHours));
        DatabasePathText().Text(m_store->DatabasePath().wstring());
        UpdateDatabaseProtection();
        LoadWorkspaceSettings();
        SetComboText(SortCombo(), settings.sortMethod);
        SetComboText(ViewCombo(), settings.viewMode);
        ThemeToggleButton().Content(box_value(EqualText(settings.themeMode, L"Dark") ? L"☀" : L"☾"));
        SortDirectionButton().Content(box_value(settings.sortDescending ? L"↓" : L"↑"));
        HighlightCheck().IsChecked(settings.highlightGroupColors);
        LoadTerminalSettings();
        ShowNotebookCheck().IsChecked(settings.notesEnabled);
        UpdateNotebook();
        RenderNoteCards();
        WindowsHelloCheck().IsChecked(settings.useWindowsHello);
        PasswordRequiredCheck().IsChecked(settings.passwordRequired);
        SetComboText(DefaultDirectionCombo(), settings.metadataRtl ? L"RTL" : L"LTR");
        if (SearchStatusFilter().SelectedIndex() < 0) SearchStatusFilter().SelectedIndex(0);
        if (StatusFilterCombo().SelectedIndex() < 0) StatusFilterCombo().SelectedIndex(0);
        BashExecutableBox().Text(settings.bashExecutable);
        m_fontsLoaded=false;
        if (SettingsView().Visibility()==Visibility::Visible) EnsureFontsLoaded();
        UpdateTagFilterButton();
        HelloButton().Visibility(settings.passwordRequired && settings.useWindowsHello ? Visibility::Visible : Visibility::Collapsed);
    }

    void MainWindow::ShowSwatch(Button const& button,std::wstring const& color)
    {
        button.Tag(box_value(color));
        StackPanel content; content.Orientation(Orientation::Horizontal); content.Spacing(8);
        Border swatch; swatch.Width(18); swatch.Height(18); swatch.CornerRadius({4});
        swatch.Background(ColorBrush(color)); swatch.BorderBrush(UiBrush(L"BorderBrush")); swatch.BorderThickness({1});
        content.Children().Append(swatch);
        auto code=MakeText(color,11);
        content.Children().Append(code);
        button.Content(content);
    }

    void MainWindow::EnsureFontsLoaded()
    {
        if (m_fontsLoaded) return;
        auto families=single_threaded_observable_vector<IInspectable>();
        auto installed=SystemFonts();
        for (auto const& look : m_store->Settings().appearance)
            for (auto const& font : look.fonts)
                if (std::ranges::find(installed,font.family)==installed.end()) installed.push_back(font.family);
        for (auto const& family : installed) families.Append(box_value(family));
        auto sizes=single_threaded_observable_vector<IInspectable>();
        for (int half=16;half<=36;++half) sizes.Append(box_value(std::to_wstring(half/2)+(half%2 ? L".5" : L"")));
        for (size_t mode=0;mode<2;++mode)
        {
            auto panel=mode ? DarkAppearancePanel() : LightAppearancePanel();
            auto& editor=m_appearanceEditors[mode];
            panel.Children().Clear(); editor={};
            auto const& look=m_store->Settings().appearance[mode];
            auto colorButton=[this](std::wstring const& value,std::wstring const& label)
            {
                Button button; ShowSwatch(button,value);
                Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(button,label);
                button.Click([weak=get_weak()](IInspectable const& sender,auto const&){if(auto self=weak.get()) self->PickTypographyColor(sender.as<Button>());});
                return button;
            };
            panel.Children().Append(MakeText(L"Fonts",15,Windows::UI::Text::FontWeights::SemiBold()));
            for (size_t i=0;i<look.fonts.size();++i)
            {
                auto const& font=look.fonts[i];
                StackPanel row; row.Orientation(Orientation::Horizontal); row.Spacing(8);
                auto label=MakeText(::HudesNative::fontRoles[i]); label.Width(135); label.VerticalAlignment(VerticalAlignment::Center); row.Children().Append(label);
                ComboBox family, size;
                family.Width(180); family.ItemsSource(families); family.SelectedItem(nullptr);
                for (auto const& item:families) if (unbox_value<hstring>(item)==font.family) { family.SelectedItem(item); break; }
                size.Width(85); size.IsEditable(true);
                auto text=to_hstring(font.size);
                for (auto const& item:sizes) size.Items().Append(item);
                auto current=box_value(text);
                bool matched{};
                for (auto const& item:size.Items()) if(unbox_value<hstring>(item)==text)
                { size.SelectedItem(item); matched=true; break; }
                if (!matched) {size.Items().Append(current); size.SelectedItem(current);}
                size.Text(text);
                Automation::AutomationProperties::SetName(family,std::wstring(::HudesNative::fontRoles[i])+L" font");
                Automation::AutomationProperties::SetName(size,std::wstring(::HudesNative::fontRoles[i])+L" size");
                row.Children().Append(family); row.Children().Append(size);
                auto button=colorButton(font.color,std::wstring(::HudesNative::fontRoles[i])+L" text color");
                if (i!=::HudesNative::GroupFont) row.Children().Append(button);
                else row.Children().Append(MakeText(L"Uses group color"));
                editor.families.push_back(family); editor.sizes.push_back(size); editor.textColors.push_back(button);
                panel.Children().Append(row);
            }
            panel.Children().Append(MakeText(L"Colors",15,Windows::UI::Text::FontWeights::SemiBold()));
            for (size_t i=0;i<look.colors.size();++i)
            {
                StackPanel row; row.Orientation(Orientation::Horizontal); row.Spacing(12);
                auto const& role=::HudesNative::paletteRoles[i];
                auto label=MakeText(role.label); label.Width(180); label.VerticalAlignment(VerticalAlignment::Center);
                auto button=colorButton(look.colors[i],role.label);
                row.Children().Append(label); row.Children().Append(button);
                panel.Children().Append(row); editor.palette.push_back(button);
            }
        }
        m_fontsLoaded=true;
    }

    void MainWindow::ReadAppearance(::HudesNative::AppSettings& settings)
    {
        if (!m_fontsLoaded) return;
        for (size_t mode=0;mode<2;++mode)
        {
            auto const& editor=m_appearanceEditors[mode];
            auto& look=settings.appearance[mode];
            for (size_t i=0;i<look.fonts.size();++i)
            {
                auto& font=look.fonts[i];
                font.family=unbox_value_or<hstring>(editor.families[i].SelectedItem(),font.family);
                std::wstring typed(editor.sizes[i].Text());
                size_t used{};
                double value=std::stod(typed,&used);
                if (used!=typed.size() || !(value>=1 && value<=200)) throw hresult_invalid_argument(L"Enter a font size from 1 to 200.");
                font.size=value;
                font.color=unbox_value<hstring>(editor.textColors[i].Tag());
            }
            for (size_t i=0;i<look.colors.size();++i) look.colors[i]=unbox_value<hstring>(editor.palette[i].Tag());
        }
    }

    void MainWindow::LoadGroups()
    {
        auto previousLoading = std::exchange(m_loadingControls, true);
        auto restoreLoading = wil::scope_exit([&] { m_loadingControls = previousLoading; });
        auto previousNote = NoteGroupCombo().SelectedItem().try_as<ComboBoxItem>();
        auto previousFilter = GroupFilterCombo().SelectedItem().try_as<ComboBoxItem>();
        auto noteId = previousNote ? unbox_value_or<hstring>(previousNote.Tag(), L"") : L"";
        auto filterId = previousFilter ? unbox_value_or<hstring>(previousFilter.Tag(), L"") : L"";
        NoteGroupCombo().Items().Clear();
        GroupFilterCombo().Items().Clear();
        ComboBoxItem ungrouped; ungrouped.Content(box_value(L"Ungrouped")); ungrouped.Tag(box_value(L"")); NoteGroupCombo().Items().Append(ungrouped);
        ComboBoxItem all; all.Content(box_value(L"All groups")); all.Tag(box_value(L"")); GroupFilterCombo().Items().Append(all);
        ComboBoxItem filterUngrouped; filterUngrouped.Content(box_value(L"Ungrouped")); filterUngrouped.Tag(box_value(L"__ungrouped")); GroupFilterCombo().Items().Append(filterUngrouped);
        for (auto const& group : m_store->Groups())
        {
            ComboBoxItem note; note.Content(box_value(group.name)); note.Tag(box_value(group.id)); NoteGroupCombo().Items().Append(note);
            ComboBoxItem filter; filter.Content(box_value(group.name)); filter.Tag(box_value(group.id)); GroupFilterCombo().Items().Append(filter);
        }
        LoadSearchFilters();
        NoteGroupCombo().SelectedIndex(0);
        GroupFilterCombo().SelectedIndex(0);
        for (auto const& raw : NoteGroupCombo().Items()) if (auto item = raw.try_as<ComboBoxItem>(); item && unbox_value_or<hstring>(item.Tag(), L"") == noteId) NoteGroupCombo().SelectedItem(item);
        for (auto const& raw : GroupFilterCombo().Items()) if (auto item = raw.try_as<ComboBoxItem>(); item && unbox_value_or<hstring>(item.Tag(), L"") == filterId) GroupFilterCombo().SelectedItem(item);
        FilterDefinitionCombo(NoteGroupCombo(), NoteGroupFind().Text());
        FilterDefinitionCombo(GroupFilterCombo(), HomeGroupFind().Text());
    }

    void MainWindow::FilterDefinitionCombo(ComboBox const& combo, hstring const& text)
    {
        if (!combo) return;
        auto query = Lower(std::wstring(text));
        for (auto const& raw : combo.Items())
        {
            auto item = raw.try_as<ComboBoxItem>();
            if (!item) continue;
            auto label = Lower(std::wstring(unbox_value_or<hstring>(item.Content(), L"")));
            auto visible = item == combo.SelectedItem() || label.find(query) != std::wstring::npos
                || label == L"all groups" || label == L"all tags" || label == L"ungrouped";
            item.Visibility(visible ? Visibility::Visible : Visibility::Collapsed);
        }
    }

    void MainWindow::DefinitionFind_Changed(IInspectable const& sender, TextChangedEventArgs const&)
    {
        auto input = sender.as<TextBox>();
        if (input == HomeGroupFind()) FilterDefinitionCombo(GroupFilterCombo(), input.Text());
        else if (input == NoteGroupFind()) FilterDefinitionCombo(NoteGroupCombo(), input.Text());
        else if (input == SearchGroupFind()) FilterDefinitionCombo(SearchGroupFilter(), input.Text());
        else if (input == SearchTagFind()) FilterDefinitionCombo(SearchTagFilter(), input.Text());
        else if (input == NoteTagFind() && TagPickerPanel())
        {
            auto query = Lower(std::wstring(input.Text()));
            for (auto const& child : TagPickerPanel().Children())
                if (auto check = child.try_as<CheckBox>())
                {
                    auto label = Lower(std::wstring(unbox_value_or<hstring>(check.Content(), L"")));
                    check.Visibility(label.find(query) != std::wstring::npos ? Visibility::Visible : Visibility::Collapsed);
                }
        }
    }

    void MainWindow::LoadTags(std::vector<std::wstring> const& selected)
    {
        TagPickerPanel().Children().Clear();
        for (auto const& tag : m_store->Tags())
        {
            if (::HudesNative::IsAutomaticTag(tag)) continue;
            CheckBox check;
            check.Content(box_value(tag));
            check.Tag(box_value(tag));
            check.IsChecked(std::ranges::any_of(selected, [&](auto const& value) { return EqualText(value, tag); }));
            check.Click([weak = get_weak()](auto const&, auto const&) { if (auto self = weak.get()) self->UpdateTagSummary(); });
            TagPickerPanel().Children().Append(check);
        }
        if (TagPickerPanel().Children().Size() == 0)
        {
            auto empty = MakeText(L"No tags created yet", 10);
            empty.Foreground(UiBrush(L"MutedBrush"));
            TagPickerPanel().Children().Append(empty);
        }
        DefinitionFind_Changed(NoteTagFind(), TextChangedEventArgs{ nullptr });
        ApplyTextDirection();
        UpdateTagSummary();
    }

    std::vector<std::wstring> MainWindow::SelectedTags()
    {
        std::vector<std::wstring> result;
        for (auto const& child : TagPickerPanel().Children())
            if (auto check = child.try_as<CheckBox>(); check && check.IsChecked() && check.IsChecked().Value())
                result.emplace_back(unbox_value_or<hstring>(check.Tag(), L"").c_str());
        return result;
    }

    void MainWindow::UpdateTagSummary()
    {
        auto tags = SelectedTags();
        std::wstring text = L"No tags";
        if (tags.size() <= 2 && !tags.empty())
        {
            text.clear();
            for (auto const& tag : tags) { if (!text.empty()) text += L"، "; text += tag; }
        }
        else if (tags.size() > 2) text = std::to_wstring(tags.size()) + L" tags selected";
        TagPickerButton().Content(box_value(text));
    }

    void MainWindow::UpdateTagFilterButton()
    {
        auto const& available = m_store->Tags();
        std::erase_if(m_tagFilters, [&](auto const& selected)
        {
            return !std::ranges::any_of(available, [&](auto const& tag) { return EqualText(selected, tag); });
        });
        if (m_tagFilters.empty()) TagFilterButton().Content(box_value(L"All tags"));
        else if (m_tagFilters.size() == 1) TagFilterButton().Content(box_value(m_tagFilters.front()));
        else TagFilterButton().Content(box_value(std::to_wstring(m_tagFilters.size()) + L" tags"));
    }

    void MainWindow::ShowTab(UIElement const& view, Button const& button)
    {
        HomeView().Visibility(Visibility::Collapsed);
        SearchView().Visibility(Visibility::Collapsed);
        SettingsView().Visibility(Visibility::Collapsed);
        view.Visibility(Visibility::Visible);
        HomeTabButton().Background(SolidColorBrush(winrt::Windows::UI::Color{}));
        SearchTabButton().Background(SolidColorBrush(winrt::Windows::UI::Color{}));
        SettingsTabButton().Background(SolidColorBrush(winrt::Windows::UI::Color{}));
        HomeTabButton().Foreground(UiBrush(L"InkBrush"));
        SearchTabButton().Foreground(UiBrush(L"InkBrush"));
        SettingsTabButton().Foreground(UiBrush(L"InkBrush"));
        button.Background(UiBrush(L"SelectedBrush"));
        button.Foreground(UiBrush(L"InkBrush"));
        view.Opacity(0.72);
        view.StartBringIntoView();
        view.Opacity(1.0);
    }

    void MainWindow::UnlockSucceeded()
    {
        CompactWindow(false);
        if (!m_workspaceInitialized)
        {
            LoadSettingsControls();
            LoadGroups();
            ApplyTextDirection();
            ApplyTypography();
            m_workspaceInitialized = true;
        }
        MainInputContainer().Visibility(Visibility::Visible);
        LoginErrorText().Text(L"");
        LoginPasswordBox().Password(L"");
        LockOverlay().Visibility(Visibility::Collapsed);
        auto const& settings = m_store->Settings();
        if (!settings.desktopFolderConfirmed || settings.desktopFolder.empty() || !std::filesystem::is_directory(settings.desktopFolder))
        {
            FirstRunOverlay().Visibility(Visibility::Visible);
            return;
        }
        m_desktop = settings.desktopFolder;
        m_startFolder = std::filesystem::is_directory(settings.defaultFolder) ? settings.defaultFolder : m_desktop;
        MainInputContainer().IsEnabled(true);
        m_ready = true;
        NavigateTo(m_startFolder, false);
        UpdateTerminalPanel();
        RefreshSearch();
    }

    void MainWindow::NavigateTo(std::filesystem::path const& folder, bool animate)
    {
        CloseDefinitionPanel();
        if (!m_noteFilter.empty()) { m_noteFilter.clear(); RenderNoteCards(); }
        if (!IsArchiveFolder(folder) && IsProtectedFolder(folder)) { NavigateTo(::HudesNative::ArchiveRoot(), animate); return; }
        if (!IsArchiveFolder(folder) && !std::filesystem::is_directory(folder)) return;
        m_currentFolder = IsArchiveFolder(folder) ? folder : std::filesystem::absolute(folder).lexically_normal();
        StartWatcher();
        RefreshFiles(animate);
    }

    void MainWindow::StartWatcher()
    {
        try
        {
            m_watcher.Start(IsArchiveFolder(m_currentFolder) ? m_desktop : m_currentFolder, [weak = get_weak()]
            {
                if (auto self = weak.get()) self->QueueRefresh();
            });
        }
        catch (...) {}
    }

    void MainWindow::QueueRefresh()
    {
        if (m_refreshQueued.exchange(true)) return;
        DispatcherQueue().TryEnqueue(winrt::Microsoft::UI::Dispatching::DispatcherQueuePriority::Low, [weak = get_weak()]
        {
            if (auto self = weak.get())
            {
                self->m_refreshQueued = false;
                if (self->m_ready) self->RefreshFiles();
            }
        });
    }

    bool MainWindow::IsArchiveFolder(std::filesystem::path const& path) const
    {
        return ::HudesNative::IsVirtualArchive(path);
    }

    bool MainWindow::IsWorkspaceLocation() const
    {
        return !m_currentFolder.empty() && !IsProtectedFolder(m_currentFolder) &&
            (CanManage(m_currentFolder) || ::HudesNative::SamePath(m_currentFolder, m_desktop));
    }

    std::wstring MainWindow::FolderLabel() const
    {
        if (!m_noteFilter.empty())
        {
            auto const& notes = m_store->PanelNotes();
            auto found = std::ranges::find(notes, m_noteFilter, &::HudesNative::PanelNote::id);
            if (found != notes.end()) return L"Linked files · " + found->title;
        }
        if (IsArchiveFolder(m_currentFolder)) return ArchiveLocationName(m_currentFolder);
        if (::HudesNative::SamePath(m_currentFolder, m_desktop)) return L"Desktop";
        if (::HudesNative::SamePath(m_currentFolder, m_startFolder)) return L"Home · " + m_startFolder.wstring();
        return m_currentFolder.wstring();
    }

    ListViewBase MainWindow::ActiveFileList()
    {
        return FileGrid().Visibility() == Visibility::Visible ? FileGrid().as<ListViewBase>() : FileDetails().as<ListViewBase>();
    }

    std::vector<std::filesystem::path> MainWindow::SelectedPaths()
    {
        std::vector<std::filesystem::path> result;
        for (auto const& raw : ActiveFileList().SelectedItems())
            if (auto element = raw.try_as<FrameworkElement>())
                if (auto path = unbox_value_or<hstring>(element.Tag(), L""); !path.empty()) result.emplace_back(path.c_str());
        return result;
    }

    std::optional<std::filesystem::path> MainWindow::SelectedOne()
    {
        auto paths = SelectedPaths();
        return paths.size() == 1 ? std::optional(paths.front()) : std::nullopt;
    }

    std::optional<std::filesystem::path> MainWindow::PreviewPath()
    {
        if (auto path = SelectedOne()) return path;
        if (!m_selectMode || !m_lastSelectedPath) return std::nullopt;
        auto selected = SelectedPaths();
        return std::ranges::any_of(selected, [&](auto const& path) { return ::HudesNative::SamePath(path, *m_lastSelectedPath); })
            ? m_lastSelectedPath : std::nullopt;
    }

    void MainWindow::EnsureContextSelection(std::filesystem::path const& path)
    {
        auto list = ActiveFileList();
        for (auto const& raw : list.SelectedItems())
        {
            if (auto element = raw.try_as<FrameworkElement>())
            {
                auto selectedPath = std::filesystem::path(unbox_value_or<hstring>(element.Tag(), L"").c_str());
                if (path == selectedPath || (!selectedPath.empty() && !IsArchiveFolder(path) && !IsArchiveFolder(selectedPath) && ::HudesNative::SamePath(path, selectedPath))) return;
            }
        }

        for (auto const& raw : list.Items())
        {
            if (auto element = raw.try_as<FrameworkElement>())
            {
                auto itemPath = std::filesystem::path(unbox_value_or<hstring>(element.Tag(), L"").c_str());
                if (path == itemPath || (!itemPath.empty() && !IsArchiveFolder(path) && !IsArchiveFolder(itemPath) && ::HudesNative::SamePath(path, itemPath)))
                {
                    list.SelectedItems().Clear();
                    list.SelectedItem(raw);
                    return;
                }
            }
        }
    }

    void MainWindow::UpdateAddress()
    {
        auto selected = SelectedPaths();
        if (selected.empty()) { AddressText().Text(FolderLabel()); return; }
        if (selected.size() == 1 && IsArchiveFolder(selected.front())) { AddressText().Text(ArchiveLocationName(selected.front())); return; }
        std::uintmax_t total{};
        bool hasFile{};
        for (auto const& path : selected)
        {
            std::error_code ignored;
            if (!IsArchiveFolder(path) && std::filesystem::is_regular_file(path, ignored))
            {
                total += std::filesystem::file_size(path, ignored);
                hasFile = true;
            }
        }
        auto size = hasFile ? ::HudesNative::FormatSize(total) : L"Folder";
        if (selected.size() == 1) AddressText().Text(selected.front().filename().wstring() + L" - " + size);
        else AddressText().Text(std::to_wstring(selected.size()) + L" items selected - " + size);
    }

    bool MainWindow::CanPinHere() const
    {
        if (!m_noteFilter.empty()) return false;
        return (!m_desktop.empty() && ::HudesNative::SamePath(m_currentFolder, m_desktop))
            || (IsArchiveFolder(m_currentFolder) && m_currentFolder != ::HudesNative::ArchiveRoot());
    }

    void MainWindow::PinSelected(bool pinned)
    {
        if (!CanPinHere()) return;
        auto paths = SelectedPaths();
        if (paths.empty() || std::ranges::any_of(paths, [&](auto const& path) { return !CanManage(path); })) return;
        try
        {
            m_store->SetPinned(paths, pinned);
            RefreshFiles();
        }
        catch (...) { ShowMessage(L"Pin items", ::HudesNative::FriendlyError()); }
    }

    void MainWindow::RefreshFiles(bool animate)
    {
        if (m_editing) { m_filesDirty=true; return; }
        if (m_fileTransfers || m_refreshingFiles) { m_filesDirty = true; return; }
        m_refreshingFiles = true;
        m_filesDirty = false;
        auto finishRefresh = wil::scope_exit([&]
        {
            m_refreshingFiles = false;
            if (m_filesDirty) QueueRefresh();
        });
        bool virtualFolder = IsArchiveFolder(m_currentFolder);
        bool workspace = !m_noteFilter.empty() || virtualFolder || IsWorkspaceLocation();
        auto managedVisibility = workspace ? Visibility::Visible : Visibility::Collapsed;
        for (UIElement const& element : std::array<UIElement, 6>{ OptionGroup(), OptionTagFilter(), OptionStatus(), OptionHighlight(), OptionGroupSelect(), OptionTagSelect() })
            element.Visibility(managedVisibility);
        NewItemButton().Visibility(IsWorkspaceLocation() ? Visibility::Visible : Visibility::Collapsed);
        GroupPanel().Visibility(managedVisibility);
        auto nameFilter = FileNameFilterBox().Text();
        auto hasNameFilter = std::ranges::any_of(nameFilter, [](wchar_t c) { return !::iswspace(c); });
        auto hasFilter = !m_noteFilter.empty() || hasNameFilter || (workspace && (!m_tagFilters.empty() || GroupFilterCombo().SelectedIndex() > 0 || StatusFilterCombo().SelectedIndex() > 0));
        ClearFileFiltersButton().Visibility(hasFilter ? Visibility::Visible : Visibility::Collapsed);
        if (m_currentFolder.empty() || (!virtualFolder && !std::filesystem::is_directory(m_currentFolder))) return;
        try
        {
            if (!virtualFolder && workspace) m_store->InheritNewChildren(m_currentFolder);
            auto selected = m_selectionToRestore.empty() ? SelectedPaths() : std::exchange(m_selectionToRestore, {});
            auto groupItem = GroupFilterCombo().SelectedItem().try_as<ComboBoxItem>();
            auto groupFilter = groupItem ? unbox_value_or<hstring>(groupItem.Tag(), L"") : L"";
            auto statusFilter = ComboText(StatusFilterCombo());
            auto showPins = CanPinHere();
            auto nameQuery = std::wstring{ nameFilter };
            m_items.clear();
            auto addArchiveFolder = [&](std::filesystem::path path, std::wstring name, std::wstring color)
            {
                ::HudesNative::FileItem item;
                item.path = std::move(path);
                item.name = std::move(name);
                item.archiveColor = std::move(color);
                item.type = L"Virtual folder";
                item.isFolder = item.isArchive = true;
                m_items.push_back(std::move(item));
            };
            std::vector<std::filesystem::directory_entry> entries;
            if (!m_noteFilter.empty())
            {
                auto const& notes = m_store->PanelNotes();
                auto found = std::ranges::find(notes, m_noteFilter, &::HudesNative::PanelNote::id);
                if (found != notes.end())
                    for (auto const& file : found->files)
                    {
                        if (!CanManage(file)) continue;
                        std::error_code err;
                        std::filesystem::directory_entry entry(file, err);
                        if (!err && entry.exists(err)) entries.push_back(std::move(entry));
                    }
            }
            else if (m_currentFolder == ::HudesNative::ArchiveRoot())
            {
                addArchiveFolder(::HudesNative::ArchiveGroupPath({}), L"unGrouped", m_store->Settings().Look().Color(L"UngroupedBrush"));
                for (auto const& group : m_store->Groups())
                    addArchiveFolder(::HudesNative::ArchiveGroupPath(group.id), group.name, group.color);
            }
            else if (virtualFolder)
            {
                for (auto const& note : m_store->Notes())
                {
                    if (!note.archived || !CanManage(note.path) || ArchiveLocation(note) != m_currentFolder) continue;
                    std::error_code error;
                    std::filesystem::directory_entry entry(note.path, error);
                    if (!error && entry.exists(error)) entries.push_back(std::move(entry));
                }
            }
            else
            {
                if (::HudesNative::SamePath(m_currentFolder, m_desktop))
                {
                    addArchiveFolder(::HudesNative::ArchiveRoot(), L"Archive", {});
                    for (auto const& note : m_store->Notes())
                    {
                        if (!note.legacyArchiveSource || note.archived || !CanManage(note.path)) continue;
                        std::error_code error;
                        std::filesystem::directory_entry entry(note.path, error);
                        if (!error && entry.exists(error)) entries.push_back(std::move(entry));
                    }
                }
                for (auto const& entry : std::filesystem::directory_iterator(m_currentFolder, std::filesystem::directory_options::skip_permission_denied))
                {
                    if (IsProtectedFolder(entry.path())) continue;
                    entries.push_back(entry);
                }
            }
            for (auto const& entry : entries)
            {
                if (!m_store->Settings().showHidden && ::HudesNative::HiddenInFileView(entry.path(),(virtualFolder || !m_noteFilter.empty())?m_desktop:std::filesystem::path{})) continue;
                ::HudesNative::FileItem item;
                item.path = entry.path();
                item.name = entry.path().filename().wstring();
                item.isFolder = entry.is_directory();
                item.managed = CanManage(item.path);
                item.type = item.isFolder ? L"Folder" : (entry.path().extension().empty() ? L"File" : Upper(entry.path().extension().wstring().substr(1)) + L" file");
                if (!item.isFolder) { std::error_code ignored; item.size = entry.file_size(ignored); }
                std::error_code ignored; item.modified = entry.last_write_time(ignored);
                auto note = item.managed ? m_store->Get(item.path) : nullptr;
                if (m_noteFilter.empty() && !virtualFolder && note && note->archived) continue;
                if (note)
                {
                    item.pinned = showPins && note->pinned;
                    item.status = note->status;
                    item.reminderNear = !item.isArchive && ::HudesNative::ReminderIsNear(note->reminderAt, m_store->Settings().reminderWarningHours);
                    if (auto group = m_store->GetGroup(note->groupId)) { item.groupName = group->name; item.groupColor = group->color; }
                }
                if (item.managed)
                {
                    if (!virtualFolder && groupFilter == L"__ungrouped" && !item.groupColor.empty()) continue;
                    if (!virtualFolder && !groupFilter.empty() && groupFilter != L"__ungrouped")
                    {
                        if (!note || !note->groupId || *note->groupId != groupFilter.c_str()) continue;
                    }
                    if (!m_tagFilters.empty())
                    {
                        if (!note || !std::ranges::any_of(m_tagFilters, [&](auto const& selected)
                        {
                            return std::ranges::any_of(note->tags, [&](auto const& tag) { return EqualText(selected, tag); });
                        })) continue;
                    }
                    if (EqualText(statusFilter, L"Emergency"))
                    {
                        if (!item.reminderNear) continue;
                    }
                    else if (!statusFilter.empty() && !EqualText(statusFilter, L"All statuses") && !EqualText(item.status, statusFilter)) continue;
                }
                if (!::HudesNative::FileNameMatches(item.name, nameQuery)) continue;
                m_items.push_back(std::move(item));
            }
            auto descending = m_store->Settings().sortDescending;
            auto method = m_store->Settings().sortMethod;
            std::ranges::sort(m_items, [&](auto const& left, auto const& right)
            {
                if (left.pinned != right.pinned) return left.pinned;
                if (left.isArchive != right.isArchive) return left.isArchive;
                if (left.isFolder != right.isFolder) return left.isFolder;
                int comparison{};
                if (EqualText(method, L"Date modified")) comparison = left.modified < right.modified ? -1 : left.modified > right.modified ? 1 : 0;
                else if (EqualText(method, L"Type")) comparison = _wcsicmp(left.type.c_str(), right.type.c_str());
                else if (EqualText(method, L"Size")) comparison = left.size < right.size ? -1 : left.size > right.size ? 1 : 0;
                else comparison = _wcsicmp(left.name.c_str(), right.name.c_str());
                if (!comparison) comparison = _wcsicmp(left.name.c_str(), right.name.c_str());
                return descending ? comparison > 0 : comparison < 0;
            });
            m_selectionToRestore = std::move(selected);
            BuildFileControls(animate);
            UpdateAddress();
            StatusBarText().Text(std::to_wstring(m_items.size()) + L" items · " + FolderLabel());
        }
        catch (...) { ShowMessage(L"Read folder", ::HudesNative::FriendlyError()); }
    }

    FrameworkElement MainWindow::BuildFileItem(::HudesNative::FileItem const& value, bool details)
    {
        (void)details;
        auto view = m_store->Settings().viewMode;
        auto large = EqualText(view, L"Large icons");
        auto smallIcons = EqualText(view, L"Small icons");
        auto listed = EqualText(view, L"List");
        auto const& settings = m_store->Settings();
        auto fileBrush = ColorBrush(settings.Look().fonts[::HudesNative::FileFont].color);

        Border card;
        card.Tag(box_value(value.path.wstring()));
        card.Margin(ThicknessHelper::FromUniformLength(5));
        card.HorizontalAlignment(HorizontalAlignment::Stretch);
        card.ContextFlyout(BuildContextMenu(value));

        if (value.isFolder && value.managed)
        {
            card.AllowDrop(true);
            card.DragOver([weak = get_weak()](IInspectable const& sender, DragEventArgs const& args)
            {
                if (auto self = weak.get()) self->FileView_DragOver(sender, args);
            });
            card.Drop([weak = get_weak()](IInspectable const& sender, DragEventArgs const& args)
            {
                if (auto self = weak.get()) self->FileView_Drop(sender, args);
            });
        }
        card.RightTapped([weak = get_weak(), path = value.path](auto const&, RightTappedRoutedEventArgs const&)
        {
            if (auto self = weak.get()) self->EnsureContextSelection(path);
        });
        card.CornerRadius({ 11 });
        card.Padding({ 8 });
        if (value.isArchive) card.Background(ColorBrush(m_store->Settings().Look().Color(L"ArchiveBrush"),55));
        else if (m_store->Settings().highlightGroupColors && !value.groupColor.empty()) card.Background(ColorBrush(value.groupColor, 46));
        else card.Background(SolidColorBrush(winrt::Windows::UI::Color{}));

        Grid iconLayer;
        auto iconSize = large ? 72.0 : smallIcons ? 48.0 : 34.0;
        iconLayer.Width(iconSize); iconLayer.Height(iconSize);
        FontIcon fallback;
        fallback.FontFamily(FontFamily(L"Segoe Fluent Icons"));
        fallback.Glyph(value.isArchive || !value.archiveColor.empty() ? L"\xE8F1" : value.isFolder ? L"\xE8B7" : L"\xE7C3");
        fallback.FontSize(large ? 48 : smallIcons ? 34 : 25);
        fallback.Foreground(!value.archiveColor.empty() ? ColorBrush(value.archiveColor) : value.isArchive ? UiBrush(L"ArchiveBrush") : UiBrush(L"AccentBrush"));
        iconLayer.Children().Append(fallback);
        Image image;
        image.Width(large ? 60 : smallIcons ? 42 : 30);
        image.Height(large ? 60 : smallIcons ? 42 : 30);
        image.Stretch(Stretch::Uniform);
        iconLayer.Children().Append(image);
        if (value.pinned)
        {
            FontIcon pin;
            pin.FontFamily(FontFamily(L"Segoe Fluent Icons"));
            pin.Glyph(L"\xE718");
            pin.FontSize(large ? 15 : 12);
            pin.Foreground(UiBrush(L"AccentBrush"));
            pin.HorizontalAlignment(HorizontalAlignment::Right);
            pin.VerticalAlignment(VerticalAlignment::Top);
            ToolTipService::SetToolTip(pin, box_value(L"Pinned"));
            iconLayer.Children().Append(pin);
        }

        if (value.managed)
        {
            Border status;
            auto diameter = large ? 21.0 : 15.0;
            status.Width(diameter); status.Height(diameter); status.CornerRadius({ diameter / 2 });
            status.Background(UiBrush(StatusColorKey(value.status)));
            status.BorderBrush(UiBrush(L"BadgeInkBrush")); status.BorderThickness({ 1.5 });
            status.HorizontalAlignment(HorizontalAlignment::Left); status.VerticalAlignment(VerticalAlignment::Bottom);
            auto glyph = MakeText(StatusGlyph(value.status), large ? 10 : 7, winrt::Windows::UI::Text::FontWeights::Bold());
            glyph.Foreground(UiBrush(L"BadgeInkBrush")); glyph.HorizontalAlignment(HorizontalAlignment::Center); glyph.VerticalAlignment(VerticalAlignment::Center);
            status.Child(glyph); iconLayer.Children().Append(status);
            if (value.reminderNear)
            {
                Border reminder;
                reminder.Width(large ? 28 : 22); reminder.Height(large ? 28 : 22); reminder.CornerRadius({ 14 });
                reminder.Background(UiBrush(L"DangerBrush")); reminder.BorderBrush(UiBrush(L"BadgeInkBrush")); reminder.BorderThickness({ 1.5 });
                reminder.HorizontalAlignment(HorizontalAlignment::Center); reminder.VerticalAlignment(VerticalAlignment::Top);
                auto clock = MakeText(L"◷", large ? 13 : 10, winrt::Windows::UI::Text::FontWeights::Bold());
                clock.Foreground(UiBrush(L"BadgeInkBrush")); clock.HorizontalAlignment(HorizontalAlignment::Center); clock.VerticalAlignment(VerticalAlignment::Center);
                reminder.Child(clock); iconLayer.Children().Append(reminder);
            }
        }

        if (large || smallIcons)
        {
            card.Width(large ? 112 : 82); card.Height(large ? 126 : 92);
            StackPanel stack;
            stack.HorizontalAlignment(HorizontalAlignment::Center);
            stack.Children().Append(iconLayer);
            auto name = MakeText(value.name, m_store->Settings().appearance[0].fonts[::HudesNative::FileFont].size, winrt::Windows::UI::Text::FontWeights::Normal());
            name.Foreground(fileBrush);
            name.TextAlignment(TextAlignment::Center); name.TextWrapping(TextWrapping::Wrap); name.MaxHeight(large ? 38 : 31); name.Width(large ? 96 : 70);
            stack.Children().Append(name);
            card.Child(stack);
        }
        else
        {
            card.Height(listed ? 54 : 48);
            card.Width(listed ? 300 : (std::max)(650.0, FileDetails().ActualWidth() - 30.0));
            Grid grid;
            ColumnDefinition iconColumn; iconColumn.Width({ 42, GridUnitType::Pixel }); grid.ColumnDefinitions().Append(iconColumn);
            ColumnDefinition nameColumn; nameColumn.Width({ 2, GridUnitType::Star }); grid.ColumnDefinitions().Append(nameColumn);
            ColumnDefinition typeColumn; typeColumn.Width({ listed ? 0.9 : 1.0, GridUnitType::Star }); grid.ColumnDefinitions().Append(typeColumn);
            ColumnDefinition sizeColumn; sizeColumn.Width({ listed ? 0.0 : 90.0, GridUnitType::Pixel }); grid.ColumnDefinitions().Append(sizeColumn);
            ColumnDefinition dateColumn; dateColumn.Width({ listed ? 0.0 : 145.0, GridUnitType::Pixel }); grid.ColumnDefinitions().Append(dateColumn);
            Grid::SetColumn(iconLayer, 0); grid.Children().Append(iconLayer);
            StackPanel nameStack; nameStack.VerticalAlignment(VerticalAlignment::Center); nameStack.Margin({ 8,0,8,0 });
            auto name = MakeText(value.name, m_store->Settings().appearance[0].fonts[::HudesNative::FileFont].size, winrt::Windows::UI::Text::FontWeights::SemiBold()); name.Foreground(fileBrush); nameStack.Children().Append(name);
            if (listed && value.managed) { auto group = MakeText(value.groupName, 10); group.Foreground(fileBrush); nameStack.Children().Append(group); }
            Grid::SetColumn(nameStack, 1); grid.Children().Append(nameStack);
            auto type = MakeText(value.type, 10); type.Foreground(fileBrush); type.VerticalAlignment(VerticalAlignment::Center); Grid::SetColumn(type, 2); grid.Children().Append(type);
            if (!listed)
            {
                auto size = MakeText(value.isFolder ? L"" : ::HudesNative::FormatSize(value.size), 10); size.Foreground(fileBrush); size.VerticalAlignment(VerticalAlignment::Center); Grid::SetColumn(size, 3); grid.Children().Append(size);
                auto date = MakeText(value.isArchive ? L"" : ::HudesNative::FileTimeText(value.modified), 10); date.Foreground(fileBrush); date.VerticalAlignment(VerticalAlignment::Center); Grid::SetColumn(date, 4); grid.Children().Append(date);
            }
            card.Child(grid);
        }

        auto tooltip = value.isArchive ? ArchiveLocationName(value.path) : value.path.wstring();
        if (auto note = value.managed ? m_store->Get(value.path) : nullptr)
        {
            if (!note->description.empty()) tooltip += L"\r\n" + note->description;
            if (!note->tags.empty())
            {
                tooltip += L"\r\n";
                for (auto const& tag : note->tags) tooltip += L"#" + tag + L" ";
            }
        }
        ToolTipService::SetToolTip(card, box_value(tooltip));
        if (!value.isArchive && value.archiveColor.empty()) LoadThumbnail(value.path, image, m_generation);
        return card;
    }

    void MainWindow::BuildFileControls(bool animate)
    {
        auto details = EqualText(m_store->Settings().viewMode, L"Details");
        ListViewBase active = details ? FileDetails().as<ListViewBase>() : FileGrid().as<ListViewBase>();
        FileGrid().Visibility(details ? Visibility::Collapsed : Visibility::Visible);
        FileDetails().Visibility(details ? Visibility::Visible : Visibility::Collapsed);
        bool restoreFileFocus = false;
        for (auto focused = FocusManager::GetFocusedElement(RootGrid().XamlRoot()).try_as<DependencyObject>();
            focused; focused = VisualTreeHelper::GetParent(focused))
            if (focused == FileSurface()) { restoreFileFocus = true; break; }
        ++m_generation;
        {
            decltype(m_storageItems) staleStorageItems;
            staleStorageItems.swap(m_storageItems);
            FileGrid().Items().Clear();
            FileDetails().Items().Clear();
        }
        auto selectionMode = m_selectMode ? ListViewSelectionMode::Multiple : ListViewSelectionMode::Extended;
        FileGrid().SelectionMode(selectionMode);
        FileDetails().SelectionMode(selectionMode);
        m_buildIndex = 0;
        BuildNextFileBatch(m_generation);
        if (restoreFileFocus) active.Focus(FocusState::Programmatic);
        if (animate)
        {
            auto visual = Microsoft::UI::Xaml::Hosting::ElementCompositionPreview::GetElementVisual(active);
            auto animation = visual.Compositor().CreateScalarKeyFrameAnimation();
            animation.InsertKeyFrame(0.0f, 0.94f);
            animation.InsertKeyFrame(1.0f, 1.0f);
            animation.Duration(std::chrono::milliseconds(130));
            visual.StartAnimation(L"Opacity", animation);
        }
    }

    void MainWindow::BuildNextFileBatch(std::uint64_t generation)
    {
        if (generation != m_generation) return;
        auto details = EqualText(m_store->Settings().viewMode, L"Details");
        auto active = ActiveFileList();
        auto const end = (std::min)(m_buildIndex + std::size_t{ 32 }, m_items.size());
        for (; m_buildIndex < end; ++m_buildIndex)
        {
            auto const& item = m_items[m_buildIndex];
            auto element = BuildFileItem(item, details);
            active.Items().Append(element);
            if (std::ranges::any_of(m_selectionToRestore, [&](auto const& path)
                { return path == item.path || (!item.isArchive && !IsArchiveFolder(path) && ::HudesNative::SamePath(path, item.path)); }))
            {
                active.SelectedItems().Append(element);
                active.ScrollIntoView(element);
            }
        }
        if (m_buildIndex < m_items.size())
        {
            DispatcherQueue().TryEnqueue(winrt::Microsoft::UI::Dispatching::DispatcherQueuePriority::Low,
                [weak = get_weak(), generation]
                {
                    if (auto self = weak.get()) self->BuildNextFileBatch(generation);
                });
        }
        else
        {
            m_selectionToRestore.clear();
        }
    }

    fire_and_forget MainWindow::LoadThumbnail(std::filesystem::path path, Image image, std::uint64_t generation)
    {
        auto lifetime = get_strong();
        try
        {
            StorageItemThumbnail thumbnail{ nullptr };
            IStorageItem storageItem{ nullptr };
            if (std::filesystem::is_directory(path))
            {
                auto folder = co_await StorageFolder::GetFolderFromPathAsync(path.wstring());
                storageItem = folder;
                if (generation != m_generation || m_windowClosed) co_return;
                m_storageItems.insert_or_assign(path.wstring(), storageItem);
                thumbnail = co_await folder.GetThumbnailAsync(ThumbnailMode::SingleItem, 96);
            }
            else
            {
                auto file = co_await StorageFile::GetFileFromPathAsync(path.wstring());
                storageItem = file;
                if (generation != m_generation || m_windowClosed) co_return;
                m_storageItems.insert_or_assign(path.wstring(), storageItem);
                thumbnail = co_await file.GetThumbnailAsync(ThumbnailMode::SingleItem, 96);
            }
            if (generation != m_generation) co_return;
            if (!thumbnail) co_return;
            BitmapImage bitmap;
            co_await bitmap.SetSourceAsync(thumbnail);
            if (generation == m_generation) image.Source(bitmap);
        }
        catch (...) {}
    }

    fire_and_forget MainWindow::CacheStorageItem(std::filesystem::path path, std::uint64_t generation)
    {
        auto lifetime = get_strong();
        try
        {
            IStorageItem item{ nullptr };
            if (std::filesystem::is_directory(path)) item = co_await StorageFolder::GetFolderFromPathAsync(path.wstring());
            else item = co_await StorageFile::GetFileFromPathAsync(path.wstring());
            if (generation == m_generation) m_storageItems.emplace(path.wstring(), item);
        }
        catch (...) {}
    }

    void MainWindow::RefreshSearch(bool updateFilters)
    {
        if (!m_store) return;
        if (updateFilters) LoadSearchFilters();
        SearchResults().Items().Clear();
        for (auto note : m_store->Search(SearchBox().Text().c_str()))
        {
            if (!CanManage(note->path) || (!m_store->Settings().showHidden && ::HudesNative::HiddenInFileView(note->path,m_desktop))) continue;
            auto groupItem = SearchGroupFilter().SelectedItem().try_as<ComboBoxItem>();
            auto selectedGroupId = groupItem ? std::wstring(unbox_value_or<hstring>(groupItem.Tag(), L"")) : std::wstring{};
            auto selectedTag = ComboText(SearchTagFilter());
            auto status = ComboText(SearchStatusFilter());
            if (selectedGroupId == L"__ungrouped" && note->groupId) continue;
            if (!selectedGroupId.empty() && selectedGroupId != L"__ungrouped" && (!note->groupId || *note->groupId != selectedGroupId)) continue;
            if (SearchTagFilter().SelectedIndex() > 0 && !std::ranges::any_of(note->tags, [&](auto const& value) { return EqualText(value, selectedTag); })) continue;
            if (status == L"Emergency") { if (!::HudesNative::ReminderIsNear(note->reminderAt, m_store->Settings().reminderWarningHours)) continue; }
            else if (!status.empty() && status != L"All statuses" && !EqualText(note->status, status)) continue;
            ListViewItem item;
            item.Tag(box_value(note->path.wstring()));
            item.Background(SolidColorBrush(winrt::Windows::UI::Color{}));
            item.Foreground(UiBrush(L"InkBrush"));
            Border card; card.Background(UiBrush(L"CardBrush")); card.BorderBrush(UiBrush(L"BorderBrush")); card.BorderThickness({ 1 }); card.CornerRadius({ 13 }); card.Padding({ 15,18,15,18 }); card.Margin({ 0,0,0,9 });
            Grid grid; ColumnDefinition contentColumn; contentColumn.Width({ 1, GridUnitType::Star }); grid.ColumnDefinitions().Append(contentColumn); ColumnDefinition buttonColumn; buttonColumn.Width({ 130, GridUnitType::Pixel }); grid.ColumnDefinitions().Append(buttonColumn);
            StackPanel stack;
            auto const& settings = m_store->Settings();
            auto searchBrush = ColorBrush(settings.Look().fonts[::HudesNative::SearchFont].color);
            auto rtl = settings.metadataRtl;
            auto title = MakeText(note->path.filename().wstring(), 12, winrt::Windows::UI::Text::FontWeights::SemiBold()); title.Foreground(searchBrush); stack.Children().Append(title);
            auto description = MakeText(note->description, settings.Look().fonts[::HudesNative::SearchFont].size); description.Foreground(searchBrush); description.TextWrapping(TextWrapping::Wrap); description.FlowDirection(rtl ? FlowDirection::RightToLeft : FlowDirection::LeftToRight); description.TextAlignment(TextAlignment::Left); description.TextReadingOrder(TextReadingOrder::UseFlowDirection); description.Language(rtl ? L"fa-IR" : L"en-US"); description.Margin({ 0,7,0,7 }); stack.Children().Append(description);
            std::wstring tags; for (auto const& tag : note->tags) { if (!tags.empty()) tags += L"  •  "; tags += tag; }
            auto tagText = MakeText(tags, settings.Look().fonts[::HudesNative::TagFont].size, winrt::Windows::UI::Text::FontWeights::Bold()); tagText.FontFamily(FontFamily(settings.Look().fonts[::HudesNative::TagFont].family)); tagText.Foreground(ColorBrush(settings.Look().fonts[::HudesNative::TagFont].color)); tagText.FlowDirection(rtl ? FlowDirection::RightToLeft : FlowDirection::LeftToRight); tagText.TextAlignment(TextAlignment::Left); tagText.TextReadingOrder(TextReadingOrder::UseFlowDirection); tagText.Language(rtl ? L"fa-IR" : L"en-US"); stack.Children().Append(tagText);
            grid.Children().Append(stack);
            StackPanel searchActions; searchActions.Margin({ 16,2,0,0 }); searchActions.VerticalAlignment(VerticalAlignment::Top);
            Button show; show.Content(box_value(L"Show in files")); show.Background(UiBrush(L"SelectedBrush")); show.Foreground(searchBrush); show.HorizontalAlignment(HorizontalAlignment::Stretch); show.Tag(box_value(note->path.wstring()));
            show.Click([weak = get_weak()](IInspectable const& sender, auto const&)
            {
                if (auto self = weak.get())
                {
                    auto button = sender.as<Button>();
                    auto path = std::filesystem::path(unbox_value<hstring>(button.Tag()).c_str());
                    self->ShowTab(self->HomeView(), self->HomeTabButton());
                    self->m_selectionToRestore = { path };
                    auto saved = self->m_store->Get(path);
                    self->NavigateTo(saved && saved->archived ? self->ArchiveLocation(*saved) :
                        saved && saved->legacyArchiveSource ? self->m_desktop : path.parent_path());
                }
            });
            searchActions.Children().Append(show);
            if (auto group = m_store->GetGroup(note->groupId))
            {
                auto groupName = MakeText(group->name, settings.Look().fonts[::HudesNative::GroupFont].size, winrt::Windows::UI::Text::FontWeights::SemiBold());
                groupName.FontFamily(FontFamily(settings.Look().fonts[::HudesNative::GroupFont].family));
                groupName.Foreground(ColorBrush(group->color));
                groupName.TextAlignment(TextAlignment::Center);
                groupName.TextWrapping(TextWrapping::Wrap);
                groupName.Margin({ 0,8,0,0 });
                searchActions.Children().Append(groupName);
            }
            Grid::SetColumn(searchActions, 1); grid.Children().Append(searchActions);
            card.Child(grid); item.Content(card); SearchResults().Items().Append(item);
        }
    }

    void MainWindow::ShowSelectedMetadata()
    {
        auto selected = SelectedPaths();
        auto count = selected.size();
        auto editable = count == 1 && CanManage(selected.front());
        auto hasPath = count == 1 && !IsProtectedFolder(selected.front());
        CopyItemPathMenu().IsEnabled(hasPath);
        CopyContainingPathMenu().IsEnabled(hasPath);
        MetadataPanel().Visibility(editable ? Visibility::Visible : Visibility::Collapsed);
        GroupPanel().Visibility((IsWorkspaceLocation() || IsArchiveFolder(m_currentFolder)) && (count != 1 || editable)
            ? Visibility::Visible : Visibility::Collapsed);
        MetadataButtons().Visibility(Visibility::Collapsed);
        if (!editable)
        {
            auto isVirtual = count == 1 && IsArchiveFolder(selected.front());
            SelectedNameText().Text(count == 1 ? (isVirtual ? ArchiveLocationName(selected.front()) : selected.front().filename().wstring())
                : count > 1 ? std::to_wstring(count) + L" items selected" : L"Nothing selected");
            SelectedPathText().Text(count == 1 ? (isVirtual ? L"Virtual folder" : selected.front().wstring())
                : count > 1 ? L"Use the context menu for batch actions." : L"Select a file to see its details.");
            SideNoteBox().Text(L"");
            NotePreviewText().Text(L"");
            NotePreviewTagsText().Text(L"");
            RenderTagChips({});
            NotePreviewPanel().Visibility(Visibility::Collapsed);
            EditNoteButton().Visibility(Visibility::Collapsed);
            SetMetadataEditing(false);
            NoteGroupCombo().SelectedIndex(0);
            LoadTags();
            SideGroupText().Text(L"");
            return;
        }
        auto const& path = selected.front();
        SelectedNameText().Text(path.filename().wstring());
        SelectedPathText().Text(path.wstring());
        auto note = m_store->Get(path);
        SideNoteBox().Text(L"");
        auto hasNote = note && !note->description.empty();
        auto hasTags = note && !note->tags.empty();
        std::wstring previewTags;
        if (hasTags) for (auto const& tag : note->tags) { if (!previewTags.empty()) previewTags += L"  "; previewTags += L"#" + tag; }
        NotePreviewText().Text(hasNote ? note->description : L"");
        NotePreviewText().Visibility(hasNote ? Visibility::Visible : Visibility::Collapsed);
        RenderTagChips(note ? note->tags : std::vector<std::wstring>{});
        NotePreviewTagsText().Text(previewTags);
        NotePreviewTagsText().Visibility(Visibility::Collapsed);
        NotePreviewPanel().Visibility(hasNote || hasTags ? Visibility::Visible : Visibility::Collapsed);
        EditNoteButton().Content(box_value(hasNote || hasTags ? L"Edit" : L"Add note"));
        EditNoteButton().Visibility(Visibility::Visible);
        NoteGroupCombo().SelectedIndex(0);
        if (note && note->groupId)
            for (auto const& raw : NoteGroupCombo().Items()) if (auto item = raw.try_as<ComboBoxItem>(); item && unbox_value_or<hstring>(item.Tag(), L"") == *note->groupId) NoteGroupCombo().SelectedItem(item);
        LoadTags(note ? note->tags : std::vector<std::wstring>{});
        SideGroupText().Text(L"Status: " + (note ? note->status : L"New"));
        SideNoteBox().Text(note ? note->description : L"");
        SetMetadataEditing(false);
    }

    void MainWindow::SetMetadataEditing(bool enabled)
    {
        m_editing = enabled;
        if(enabled) m_metadataPath=SelectedOne().value_or(std::filesystem::path{});
        else
        {
            m_metadataPath.clear();
            if(m_filesDirty && !m_refreshingFiles && m_ready) QueueRefresh();
        }
        SideNoteBox().IsEnabled(enabled);
        bool groupEditable=enabled && !m_store->GroupInherited(m_metadataPath);
        NoteGroupCombo().IsEnabled(groupEditable);
        NoteGroupFind().Visibility(groupEditable ? Visibility::Visible : Visibility::Collapsed);
        if (!enabled) NoteGroupFind().Text(L"");
        TagPickerButton().IsEnabled(enabled);
        MetadataButtons().Visibility(enabled ? Visibility::Visible : Visibility::Collapsed);
        if (!enabled) TagPickerFlyout().Hide();
    }

    bool MainWindow::ResolveMetadataDraft()
    {
        if(!m_editing || m_metadataPath.empty()) return true;
        if(m_resolvingMetadata) return false;
        auto path=m_metadataPath;
        auto note=m_store->Get(path);
        auto groupItem=NoteGroupCombo().SelectedItem().try_as<ComboBoxItem>();
        auto name=groupItem?std::wstring(unbox_value_or<hstring>(groupItem.Tag(),L"")):std::wstring{};
        auto group=name.empty()?std::nullopt:std::optional(name);
        auto tags=SelectedTags();
        auto previousTags=note?note->tags:std::vector<std::wstring>{};
        std::erase_if(previousTags,::HudesNative::IsAutomaticTag);
        std::ranges::sort(previousTags); auto sorted=tags; std::ranges::sort(sorted);
        if(SideNoteBox().Text()==(note?note->description:std::wstring{}) && previousTags==sorted && group==(note?note->groupId:std::nullopt))
            return true;
        m_resolvingMetadata=true;
        auto reset=wil::scope_exit([&]{m_resolvingMetadata=false;});
        auto choice=AskToSave(L"the file note");
        if(choice==IDCANCEL) return false;
        if(choice==IDYES)
        {
            try {m_store->SaveNote(path,std::wstring(SideNoteBox().Text()),tags,group);}
            catch (...) {ShowMessage(L"File note",::HudesNative::FriendlyError());return false;}
        }
        SetMetadataEditing(false);
        ShowSelectedMetadata(); QueueRefresh();
        return true;
    }

    void MainWindow::BeginMetadataEdit(bool focusEditor)
    {
        auto path = SelectedOne();
        if (!path || !CanManage(*path)) return;
        if (auto note = m_store->Get(*path)) SideNoteBox().Text(note->description);
        SetMetadataEditing(true);
        EditNoteButton().Visibility(Visibility::Collapsed);
        m_detailsOpen=true; UpdateNotebook();
        if (focusEditor) SideNoteBox().Focus(FocusState::Programmatic);
    }

    MenuFlyout MainWindow::BuildContextMenu(::HudesNative::FileItem const& value)
    {
        MenuFlyout menu;
        auto weak = get_weak();
        auto contextPath = value.path;
        auto contextNote = value.managed ? m_store->Get(value.path) : nullptr;
        auto add = [&](std::wstring const& label, auto action, bool enabled = true)
        {
            MenuFlyoutItem item; item.Text(label); item.IsEnabled(enabled); item.Click([weak, contextPath, action](auto const&, auto const&) { if (auto self = weak.get()) { self->EnsureContextSelection(contextPath); (self.get()->*action)(); } }); menu.Items().Append(item);
        };
        auto copyItem = [&]
        {
            MenuFlyoutItem copy;
            copy.Text(L"Copy");
            copy.KeyboardAcceleratorTextOverride(L"Ctrl+C");
            copy.Click([weak, contextPath](auto const&, auto const&)
            {
                if (auto self = weak.get()) { self->EnsureContextSelection(contextPath); self->CopySelected(false); }
            });
            menu.Items().Append(copy);
        };
        auto metadataItem = [&]
        {
            MenuFlyoutItem item;
            item.Text(L"Open metadata window…");
            item.Click([weak, contextPath](auto const&, auto const&)
            {
                if (auto self = weak.get())
                {
                    self->EnsureContextSelection(contextPath);
                    if (auto path = self->SelectedOne()) self->MetadataDialog(*path);
                }
            });
            menu.Items().Append(item);
        };
        auto addHash = [&](wchar_t const* label, ::HudesNative::FileHashAlgorithm algorithm)
        {
            MenuFlyoutItem item;
            item.Text(label);
            item.Click([weak, contextPath, algorithm](auto const&, auto const&)
            {
                if (auto self = weak.get()) { self->EnsureContextSelection(contextPath); self->CalculateSelectedHash(algorithm); }
            });
            menu.Items().Append(item);
        };
        auto hashItems = [&]
        {
            if (value.isFolder) return;
            addHash(L"Calculate MD5", ::HudesNative::FileHashAlgorithm::Md5);
            addHash(L"Calculate SHA-256", ::HudesNative::FileHashAlgorithm::Sha256);
        };
        if (value.isArchive)
        {
            copyItem();
            return menu;
        }
        add(L"Open", &MainWindow::OpenSelected);
        if (!value.isFolder) add(L"Open with…", &MainWindow::OpenWithSelected);
        AppendViewMenu(menu,value.isFolder?value.path:value.path.parent_path());
        if (!value.managed)
        {
            copyItem();
            hashItems();
            metadataItem();
            add(L"Rename", &MainWindow::RenameDialog);
            add(L"Delete", &MainWindow::DeleteSelected);
            return menu;
        }
        menu.Items().Append(MenuFlyoutSeparator());
        MenuFlyoutItem edit; edit.Text(L"Edit note in side panel"); edit.IsEnabled(!value.isArchive); edit.Click([weak, contextPath](auto const&, auto const&) { if (auto self = weak.get()) { self->EnsureContextSelection(contextPath); self->BeginMetadataEdit(); } }); menu.Items().Append(edit);
        metadataItem();

        auto groupTitle = contextNote && contextNote->groupId ? L"Change group" : L"Add to group";
        MenuFlyoutItem groups;
        groups.Text(groupTitle);
        groups.IsEnabled(!m_store->Groups().empty() && !m_store->GroupInherited(contextPath));
        groups.Click([weak, contextPath, title = std::wstring(groupTitle)](auto const&, auto const&)
        {
            if (auto self = weak.get())
            {
                self->EnsureContextSelection(contextPath);
                auto paths = self->SelectedPaths();
                std::vector<std::pair<std::wstring, std::wstring>> definitions;
                for (auto const& group : self->m_store->Groups()) definitions.emplace_back(group.name, group.id);
                std::vector<std::wstring> selected;
                if (auto note = self->m_store->Get(contextPath); note && note->groupId) selected.push_back(*note->groupId);
                self->ShowDefinitionChoices(title, L"Find a group", L"Apply group", definitions, selected,
                    [weak, paths = std::move(paths)](auto values)
                    {
                        if (auto window = weak.get(); window && !values.empty())
                            window->UpdateRecords(1, paths, {}, {}, values.front());
                    }, true);
            }
        });
        menu.Items().Append(groups);
        MenuFlyoutItem removeGroup; removeGroup.Text(L"Remove from group"); removeGroup.IsEnabled(!value.isArchive && !m_store->GroupInherited(contextPath) && contextNote && contextNote->groupId.has_value()); removeGroup.Click([weak, contextPath](auto const&, auto const&) { if (auto self = weak.get()) { self->EnsureContextSelection(contextPath); self->SetGroupSelected(std::nullopt); } }); menu.Items().Append(removeGroup);

        if (CanPinHere())
        {
            MenuFlyoutItem pin;
            pin.Text(value.pinned ? L"Unpin" : L"Pin to top");
            pin.Click([weak, contextPath, pinned = !value.pinned](auto const&, auto const&)
            {
                if (auto self = weak.get())
                {
                    self->EnsureContextSelection(contextPath);
                    self->PinSelected(pinned);
                }
            });
            menu.Items().Append(pin);
        }

        MenuFlyoutSubItem status; status.Text(L"Status"); status.IsEnabled(!value.isArchive);
        for (auto const& choice : { std::wstring(L"Done"), std::wstring(L"Ongoing"), std::wstring(L"New"), std::wstring(L"Important") })
        {
            MenuFlyoutItem item; item.Text(StatusGlyph(choice) + L"  " + choice);
            if (EqualText(choice, L"New"))
            {
                auto note = m_store->Get(value.path);
                (void)note; item.IsEnabled(true);
            }
            item.Click([weak, contextPath, choice](auto const&, auto const&) { if (auto self = weak.get()) { self->EnsureContextSelection(contextPath); self->SetStatusSelected(choice); } }); status.Items().Append(item);
        }
        menu.Items().Append(status);
        MenuFlyoutItem reminder; reminder.Text(L"Reminder…"); reminder.IsEnabled(!value.isArchive); reminder.Click([weak, contextPath](auto const&, auto const&) { if (auto self = weak.get()) { self->EnsureContextSelection(contextPath); self->ReminderDialog(); } }); menu.Items().Append(reminder);
        MenuFlyoutItem clearReminder; clearReminder.Text(L"Clear reminder"); clearReminder.IsEnabled(!value.isArchive && contextNote && contextNote->reminderAt.has_value()); clearReminder.Click([weak, contextPath](auto const&, auto const&) { if (auto self = weak.get()) { self->EnsureContextSelection(contextPath); self->ClearReminderSelected(); } }); menu.Items().Append(clearReminder);

        menu.Items().Append(MenuFlyoutSeparator());
        if (contextNote && contextNote->archived)
        {
            MenuFlyoutItem unarchive; unarchive.Text(L"Unarchive");
            unarchive.Click([weak, contextPath](auto const&, auto const&) { if (auto self = weak.get()) { self->EnsureContextSelection(contextPath); self->UnarchiveSelected(); } });
            menu.Items().Append(unarchive);
        }
        else
        {
            MenuFlyoutSubItem archive; archive.Text(L"Archive"); archive.IsEnabled(!value.isArchive);
            MenuFlyoutItem moveArchive; moveArchive.Text(L"Move to archive"); moveArchive.Click([weak, contextPath](auto const&, auto const&) { if (auto self = weak.get()) { self->EnsureContextSelection(contextPath); self->ArchiveSelected(false); } }); archive.Items().Append(moveArchive);
            MenuFlyoutItem zipArchive; zipArchive.Text(L"Archive a ZIP copy…"); zipArchive.Click([weak, contextPath](auto const&, auto const&) { if (auto self = weak.get()) { self->EnsureContextSelection(contextPath); self->ArchiveSelected(true); } }); archive.Items().Append(zipArchive);
            menu.Items().Append(archive);
        }
        hashItems();
        MenuFlyoutItem createZip; createZip.Text(L"Create ZIP…"); createZip.Click([weak, contextPath](auto const&, auto const&) { if (auto self = weak.get()) { self->EnsureContextSelection(contextPath); self->CreateZipDialog(); } }); menu.Items().Append(createZip);
        MenuFlyoutItem extractZip; extractZip.Text(L"Extract ZIP…"); extractZip.IsEnabled(!value.isFolder && Lower(value.path.wstring()).ends_with(L".zip")); extractZip.Click([weak, contextPath](auto const&, auto const&) { if (auto self = weak.get()) { self->EnsureContextSelection(contextPath); self->ExtractZipDialog(); } }); menu.Items().Append(extractZip);

        menu.Items().Append(MenuFlyoutSeparator());
        MenuFlyoutItem cut; cut.Text(L"Cut"); cut.KeyboardAcceleratorTextOverride(L"Ctrl+X"); cut.Click([weak, contextPath](auto const&, auto const&) { if (auto self = weak.get()) { self->EnsureContextSelection(contextPath); self->CopySelected(true); } }); menu.Items().Append(cut);
        copyItem();
        MenuFlyoutItem rename; rename.Text(L"Rename"); rename.KeyboardAcceleratorTextOverride(L"F2"); rename.Click([weak, contextPath](auto const&, auto const&) { if (auto self = weak.get()) { self->EnsureContextSelection(contextPath); self->RenameDialog(); } }); menu.Items().Append(rename);
        MenuFlyoutItem remove; remove.Text(L"Delete"); remove.KeyboardAcceleratorTextOverride(L"Del"); remove.Click([weak, contextPath](auto const&, auto const&) { if (auto self = weak.get()) { self->EnsureContextSelection(contextPath); self->DeleteSelected(); } }); menu.Items().Append(remove);
        menu.Items().Append(MenuFlyoutSeparator());
        menu.Items().Append(NewItemSubmenu());
        return menu;
    }

    void MainWindow::PopulateNewItemMenu(Windows::Foundation::Collections::IVector<MenuFlyoutItemBase> const& items)
    {
        auto weak = get_weak();
        MenuFlyoutItem folder;
        folder.Text(L"New folder…");
        folder.Click([weak](auto const&, auto const&) { if (auto self = weak.get()) self->CreateItemDialog(true); });
        items.Append(folder);

        MenuFlyoutItem file;
        file.Text(L"New file…");
        file.Click([weak](auto const&, auto const&) { if (auto self = weak.get()) self->CreateItemDialog(false); });
        items.Append(file);
    }

    MenuFlyoutSubItem MainWindow::NewItemSubmenu()
    {
        MenuFlyoutSubItem submenu;
        submenu.Text(L"New");
        PopulateNewItemMenu(submenu.Items());
        return submenu;
    }

    void MainWindow::OpenSelected()
    {
        auto path = SelectedOne(); if (!path) return;
        try { if (IsArchiveFolder(*path) || std::filesystem::is_directory(*path)) NavigateTo(*path); else ::HudesNative::OpenPath(*path); }
        catch (...) { ShowMessage(L"Open", ::HudesNative::FriendlyError()); }
    }

    void MainWindow::OpenWithSelected()
    {
        auto path = SelectedOne(); if (!path || IsProtectedFolder(*path)) return;
        try { ::HudesNative::ShowOpenWith(WindowHandle(), *path); }
        catch (...) { ShowMessage(L"Open with", ::HudesNative::FriendlyError()); }
    }

    void MainWindow::CopySelected(bool cut)
    {
        auto paths = SelectedPaths();
        if (paths.empty()) return;
        if (!cut && paths.size() == 1 && IsArchiveFolder(paths.front())) { ExportOrCopyArchive(paths.front()); return; }
        if (std::ranges::any_of(paths, [&](auto const& path) { return IsProtectedFolder(path) || (cut && !CanManage(path)); })) return;
        try { ::HudesNative::SetClipboardFiles(WindowHandle(), paths, cut); StatusBarText().Text(cut ? L"Ready to move" : L"Copied to clipboard"); }
        catch (...) { ShowMessage(L"Clipboard", ::HudesNative::FriendlyError()); }
    }

    fire_and_forget MainWindow::PasteFiles()
    {
        if (!IsWorkspaceLocation()) co_return;
        auto lifetime = get_strong();
        bool move{};
        std::vector<std::filesystem::path> paths;
        try { paths = ::HudesNative::GetClipboardFiles(move); }
        catch (...) { ShowMessage(L"Paste", ::HudesNative::FriendlyError()); co_return; }
        if (paths.empty() || std::ranges::any_of(paths, [&](auto const& path) { return IsProtectedFolder(path); })) co_return;
        auto folder = m_currentFolder;
        ++m_fileTransfers;
        auto ui = apartment_context();
        std::exception_ptr error;
        std::vector<std::pair<std::filesystem::path, std::filesystem::path>> moved;
        co_await resume_background();
        try
        {
            for (auto const& path : paths)
            {
                if (move && ::HudesNative::SamePath(path.parent_path(), folder)) continue;
                auto destination = ::HudesNative::CopyToFolder(path, folder, move);
                if (move) moved.emplace_back(path, std::move(destination));
            }
        }
        catch (...) { error = std::current_exception(); }
        co_await ui;
        try { if (!moved.empty()) m_store->MovePaths(moved); }
        catch (...) { if (!error) error = std::current_exception(); }
        --m_fileTransfers;
        RefreshFiles();
        if (!moved.empty()) RefreshSearch();
        if (error)
        {
            try { std::rethrow_exception(error); }
            catch (...) { ShowMessage(L"Paste", ::HudesNative::FriendlyError()); }
            co_return;
        }
    }

    void MainWindow::SetGroupSelected(std::optional<std::wstring> groupId)
    {
        auto paths = SelectedPaths(); if (paths.empty()) return;
        UpdateRecords(1, std::move(paths), {}, {}, std::move(groupId));
    }

    void MainWindow::SetStatusSelected(std::wstring const& status)
    {
        auto paths = SelectedPaths(); if (paths.empty()) return;
        UpdateRecords(2, std::move(paths), {}, {}, {}, status);
    }

    void MainWindow::ClearReminderSelected()
    {
        auto paths = SelectedPaths(); if (paths.empty()) return;
        if (std::ranges::any_of(paths, [&](auto const& path) { return !CanManage(path); })) return;
        try { m_store->SetReminder(paths, std::nullopt); RefreshFiles(); }
        catch (...) { ShowMessage(L"Reminder", ::HudesNative::FriendlyError()); }
    }

    fire_and_forget MainWindow::CalculateSelectedHash(::HudesNative::FileHashAlgorithm algorithm)
    {
        auto lifetime = get_strong();
        auto path = SelectedOne();
        if (!path) co_return;
        std::wstring label = algorithm == ::HudesNative::FileHashAlgorithm::Sha256 ? L"SHA-256" : L"MD5";
        auto ui = apartment_context();
        std::wstring hash;
        std::exception_ptr error;
        co_await resume_background();
        try { hash = ::HudesNative::CalculateFileHash(*path, algorithm); }
        catch (...) { error = std::current_exception(); }
        co_await ui;
        if (error)
        {
            ShowMessage(label, ::HudesNative::FriendlyError(error));
            co_return;
        }
        try
        {
            ContentDialog dialog;
            dialog.XamlRoot(RootGrid().XamlRoot());
            dialog.Title(box_value(label + L" · " + path->filename().wstring()));
            dialog.CloseButtonText(L"Close");

            StackPanel content;
            content.Spacing(9);
            TextBox calculated;
            calculated.Header(box_value(L"Calculated " + label));
            calculated.Text(hash);
            calculated.IsReadOnly(true);
            calculated.TextWrapping(TextWrapping::Wrap);
            calculated.FontFamily(FontFamily(L"Consolas"));
            content.Children().Append(calculated);

            TextBox entered;
            entered.Header(box_value(label + L" to compare"));
            entered.PlaceholderText(L"Paste or type a hash value");
            entered.FontFamily(FontFamily(L"Consolas"));
            content.Children().Append(entered);

            Button compare;
            compare.Content(box_value(L"Compare"));
            compare.HorizontalAlignment(HorizontalAlignment::Left);
            compare.Background(UiBrush(L"SelectedBrush"));
            content.Children().Append(compare);

            TextBlock result;
            result.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            result.Visibility(Visibility::Collapsed);
            content.Children().Append(result);

            compare.Click([entered, result, good=UiBrush(L"DoneBrush"), bad=UiBrush(L"DangerBrush"), expected = Lower(hash)](auto const&, auto const&)
            {
                auto value = Lower(std::wstring(entered.Text()));
                std::erase_if(value, [](wchar_t character) { return ::iswspace(character) != 0; });
                auto valid = value == expected;
                result.Text(valid ? L"✓  Hash matches" : L"✕  Hash does not match");
                result.Foreground(valid ? good : bad);
                result.Visibility(Visibility::Visible);
            });

            dialog.Content(content);
            co_await dialog.ShowAsync();
        }
        catch (...) { ShowMessage(label, ::HudesNative::FriendlyError()); }
    }

    fire_and_forget MainWindow::ShowMessage(std::wstring title, std::wstring message)
    {
        auto lifetime = get_strong();
        try
        {
            ContentDialog dialog;
            dialog.XamlRoot(RootGrid().XamlRoot());
            dialog.Title(box_value(title));
            TextBlock content; content.Text(message); content.TextWrapping(TextWrapping::Wrap); content.IsTextSelectionEnabled(true);
            dialog.Content(content);
            dialog.CloseButtonText(L"OK");
            co_await dialog.ShowAsync();
        }
        catch (...) {}
    }

    fire_and_forget MainWindow::CreateGroupDialog()
    {
        auto lifetime = get_strong();
        CloseDefinitionPanel();
        try
        {
            ContentDialog dialog;
            dialog.XamlRoot(RootGrid().XamlRoot());
            dialog.Title(box_value(L"Manage groups"));
            dialog.PrimaryButtonText(L"Create group");
            dialog.CloseButtonText(L"Close");
            dialog.DefaultButton(ContentDialogButton::Primary);

            StackPanel fields;
            fields.Spacing(12);
            fields.MinWidth(350);
            ComboBox existing;
            existing.Header(box_value(L"Group to edit"));
            existing.HorizontalAlignment(HorizontalAlignment::Stretch);
            ComboBoxItem create;
            create.Content(box_value(L"New group"));
            existing.Items().Append(create);
            for (auto const& group : m_store->Groups())
            {
                ComboBoxItem item;
                StackPanel label;
                label.Orientation(Orientation::Horizontal);
                label.Spacing(8);
                Border swatch;
                swatch.Background(ColorBrush(group.color));
                swatch.CornerRadius({ 5 });
                swatch.Width(10); swatch.Height(10); swatch.VerticalAlignment(VerticalAlignment::Center);
                label.Children().Append(swatch);
                label.Children().Append(MakeText(group.name, 13));
                item.Content(label);
                item.Tag(box_value(group.id));
                existing.Items().Append(item);
            }
            existing.SelectedItem(create);
            fields.Children().Append(existing);

            TextBox name;
            name.Header(box_value(L"Group name"));
            name.PlaceholderText(L"Enter a name");
            fields.Children().Append(name);
            ColorPicker color;
            color.IsAlphaEnabled(false);
            color.Color(ParseColor(L"#4D91C9"));
            fields.Children().Append(color);
            TextBlock error;
            error.Foreground(UiBrush(L"DangerBrush"));
            error.TextWrapping(TextWrapping::Wrap);
            fields.Children().Append(error);
            Button remove;
            remove.Content(box_value(L"Delete group"));
            remove.Visibility(Visibility::Collapsed);
            fields.Children().Append(remove);
            existing.SelectionChanged([this, weakDialog = make_weak(dialog), name, color, remove, error](IInspectable const& sender, auto const&)
            {
                auto item = sender.as<ComboBox>().SelectedItem().try_as<ComboBoxItem>();
                auto id = item ? std::wstring(unbox_value_or<hstring>(item.Tag(), L"")) : std::wstring{};
                auto group = m_store->GetGroup(id.empty() ? std::nullopt : std::optional(id));
                name.Text(group ? group->name : L"");
                color.Color(ParseColor(group ? group->color : L"#4D91C9"));
                error.Text(L"");
                remove.Visibility(group ? Visibility::Visible : Visibility::Collapsed);
                remove.IsEnabled(group && !m_store->GroupInUse(id));
                if (auto owner = weakDialog.get()) owner.PrimaryButtonText(group ? L"Save changes" : L"Create group");
            });
            remove.Click([this, weakExisting = make_weak(existing), create, error](auto const&, auto const&)
            {
                auto existing = weakExisting.get();
                if (!existing) return;
                auto item = existing.SelectedItem().try_as<ComboBoxItem>();
                if (!item) return;
                try
                {
                    auto id = std::wstring(unbox_value_or<hstring>(item.Tag(), L""));
                    if (!m_store->DeleteGroup(id)) return;
                    uint32_t index{};
                    if (existing.Items().IndexOf(item, index)) existing.Items().RemoveAt(index);
                    existing.SelectedItem(create);
                }
                catch (...) { error.Text(::HudesNative::FriendlyError()); }
            });
            std::optional<std::wstring> created;
            auto commitGroup=[this, existing, name, color, error, &created]()
            {
                auto item = existing.SelectedItem().try_as<ComboBoxItem>();
                auto id = item ? unbox_value_or<hstring>(item.Tag(), L"") : hstring{};
                try
                {
                    if (id.empty()) created = m_store->AddGroup(std::wstring(name.Text()), ColorHex(color.Color())).id;
                    else m_store->UpdateGroup(std::wstring(id), std::wstring(name.Text()), ColorHex(color.Color()));
                }
                catch (...)
                {
                    error.Text(::HudesNative::FriendlyError()); return false;
                }
                return true;
            };
            auto dirtyGroup=[this,existing,name,color]
            {
                auto item=existing.SelectedItem().as<ComboBoxItem>();
                auto id=std::wstring(unbox_value_or<hstring>(item.Tag(),L""));
                auto original=m_store->GetGroup(id.empty()?std::nullopt:std::optional(id));
                return name.Text()!=(original?original->name:std::wstring{}) ||
                    !EqualText(ColorHex(color.Color()),original?original->color:L"#4D91C9");
            };
            ScrollViewer scroll;
            scroll.MaxHeight(570);
            scroll.Content(fields);
            dialog.Content(scroll);
            co_await ShowLightEditor(dialog,ManageGroupsButton(),dirtyGroup,commitGroup);
            LoadGroups();
            if (m_editing && created)
                for (auto const& raw : NoteGroupCombo().Items())
                    if (auto item = raw.try_as<ComboBoxItem>(); item && unbox_value_or<hstring>(item.Tag(), L"") == *created)
                        NoteGroupCombo().SelectedItem(item);
            RefreshFiles();
            RefreshSearch();
        }
        catch (...) { ShowMessage(L"Groups", ::HudesNative::FriendlyError()); }
    }

    fire_and_forget MainWindow::CreateTagsDialog()
    {
        auto lifetime = get_strong();
        CloseDefinitionPanel();
        try
        {
            Flyout editor;
            StackPanel content;
            content.MinWidth(350);
            content.Children().Append(MakeText(L"Manage tags", 19, winrt::Windows::UI::Text::FontWeights::SemiBold()));
            content.Spacing(10);
            Grid form;
            form.ColumnSpacing(8);
            ColumnDefinition fill;
            fill.Width({ 1, GridUnitType::Star });
            form.ColumnDefinitions().Append(fill);
            ColumnDefinition actions;
            actions.Width(GridLengthHelper::Auto());
            form.ColumnDefinitions().Append(actions);
            TextBox input;
            input.PlaceholderText(L"Add tags, separated by commas");
            input.FlowDirection(m_store->Settings().metadataRtl ? FlowDirection::RightToLeft : FlowDirection::LeftToRight);
            form.Children().Append(input);
            Button save;
            save.Content(box_value(L"Add tags"));
            save.Background(UiBrush(L"SelectedBrush"));
            Grid::SetColumn(save, 1);
            form.Children().Append(save);
            content.Children().Append(form);
            TextBlock error;
            error.Foreground(UiBrush(L"DangerBrush"));
            error.TextWrapping(TextWrapping::Wrap);
            error.Visibility(Visibility::Collapsed);
            content.Children().Append(error);
            auto report = [error](std::wstring const& message)
            {
                error.Text(message);
                error.Visibility(message.empty() ? Visibility::Collapsed : Visibility::Visible);
            };

            auto hint = MakeText(L"Drag the handles to reorder. Select Edit to rename a tag.", 12);
            hint.TextWrapping(TextWrapping::Wrap);
            hint.Foreground(UiBrush(L"MutedBrush"));
            content.Children().Append(hint);
            StackPanel rows;
            rows.Spacing(4);
            ScrollViewer scroll;
            scroll.MaxHeight(315);
            scroll.Content(rows);
            content.Children().Append(scroll);
            auto automatic = MakeText(L"Automatic tags   Folder · SubFolder · SubFile", 12);
            automatic.Foreground(UiBrush(L"MutedBrush"));
            content.Children().Append(automatic);

            auto editing = std::make_shared<std::wstring>();
            auto dragged = std::make_shared<std::wstring>();
            auto reload = std::make_shared<std::function<void()>>();
            auto weakReload = std::weak_ptr(reload);
            auto selected = SelectedTags();
            auto searchTag = ComboText(SearchTagFilter());
            auto reset = [editing, weakInput = make_weak(input), weakSave = make_weak(save), report]
            {
                editing->clear();
                if (auto input = weakInput.get())
                {
                    input.Text(L"");
                    input.PlaceholderText(L"Add tags, separated by commas");
                }
                if (auto save = weakSave.get()) save.Content(box_value(L"Add tags"));
                report(L"");
            };
            auto updateReferences = [&](std::wstring const& oldName, std::wstring const& replacement)
            {
                auto update = [&](auto& names)
                {
                    if (replacement.empty()) std::erase_if(names, [&](auto const& name) { return EqualText(name, oldName); });
                    else for (auto& name : names) if (EqualText(name, oldName)) name = replacement;
                };
                update(selected);
                update(m_tagFilters);
                if (EqualText(searchTag, oldName)) searchTag = replacement;
            };

            *reload = [this, rows, editing, dragged, input, save, report, reset, weakReload, &updateReferences]
            {
                rows.Children().Clear();
                for (auto const& tag : m_store->Tags())
                {
                    if (::HudesNative::IsAutomaticTag(tag)) continue;
                    Border card;
                    card.Background(UiBrush(L"ControlBrush"));
                    card.CornerRadius({ 8 });
                    card.Padding({ 8, 4, 8, 4 });
                    Grid row;
                    row.ColumnSpacing(8);
                    for (auto width : { GridLengthHelper::Auto(), GridLength{ 1, GridUnitType::Star },
                        GridLengthHelper::Auto(), GridLengthHelper::Auto() })
                    {
                        ColumnDefinition column;
                        column.Width(width);
                        row.ColumnDefinitions().Append(column);
                    }
                    FontIcon grip;
                    grip.Glyph(L"\xE76F");
                    grip.FontSize(15);
                    Border handle;
                    handle.Background(SolidColorBrush(winrt::Windows::UI::Color{}));
                    handle.Padding(ThicknessHelper::FromUniformLength(6));
                    handle.Child(grip);
                    handle.CanDrag(true);
                    ToolTipService::SetToolTip(handle, box_value(L"Drag to reorder"));
                    handle.DragStarting([dragged, tag](auto const&, DragStartingEventArgs const& args)
                    {
                        *dragged = tag;
                        args.Data().SetData(L"Hudes.TagOrder", box_value(true));
                        args.Data().RequestedOperation(DataPackageOperation::Move);
                    });
                    row.Children().Append(handle);
                    auto label = MakeText(tag, 13);
                    label.TextTrimming(TextTrimming::CharacterEllipsis);
                    label.VerticalAlignment(VerticalAlignment::Center);
                    Grid::SetColumn(label, 1);
                    row.Children().Append(label);
                    Button edit;
                    edit.Content(box_value(L"Edit"));
                    Grid::SetColumn(edit, 2);
                    edit.Click([editing, input, save, report, tag](auto const&, auto const&)
                    {
                        *editing = tag;
                        report(L"");
                        input.Text(tag);
                        save.Content(box_value(L"Save name"));
                        input.Focus(FocusState::Programmatic);
                        input.SelectAll();
                    });
                    row.Children().Append(edit);
                    Button remove;
                    remove.Content(box_value(L"Delete"));
                    Grid::SetColumn(remove, 3);
                    remove.Click([this, tag, editing, reset, weakReload, report, &updateReferences](auto const&, auto const&)
                    {
                        try
                        {
                            m_store->DeleteTag(tag);
                            updateReferences(tag, L"");
                            if (EqualText(*editing, tag)) reset();
                            if (auto rebuild = weakReload.lock()) (*rebuild)();
                        }
                        catch (...) { report(::HudesNative::FriendlyError()); }
                    });
                    row.Children().Append(remove);
                    card.Child(row);
                    card.AllowDrop(true);
                    card.DragOver([tag, dragged](auto const&, DragEventArgs const& args)
                    {
                        if (!args.DataView().Contains(L"Hudes.TagOrder")) return;
                        args.AcceptedOperation(*dragged == tag ? DataPackageOperation::None : DataPackageOperation::Move);
                        args.DragUIOverride().Caption(L"Move near " + tag);
                        args.Handled(true);
                    });
                    card.Drop([this, tag, dragged, weakReload, report](auto const&, DragEventArgs const& args)
                    {
                        if (!args.DataView().Contains(L"Hudes.TagOrder")) return;
                        args.Handled(true);
                        try
                        {
                            m_store->ReorderTag(*dragged, tag);
                            if (auto rebuild = weakReload.lock()) (*rebuild)();
                        }
                        catch (...) { report(::HudesNative::FriendlyError()); }
                    });
                    rows.Children().Append(card);
                }
            };
            (*reload)();

            auto commit = [this, editing, weakInput = make_weak(input), reset, report, weakReload, &updateReferences]
            {
                auto input = weakInput.get();
                if (!input) return;
                try
                {
                    auto text = std::wstring(input.Text());
                    if (editing->empty())
                    {
                        std::wstring tag;
                        for (auto character : text + L",")
                        {
                            if (std::wstring_view(L",،;\r\n").find(character) == std::wstring_view::npos) tag.push_back(character);
                            else
                            {
                                if (std::ranges::any_of(tag, [](auto c) { return !::iswspace(c); })) m_store->AddTag(tag);
                                tag.clear();
                            }
                        }
                    }
                    else updateReferences(*editing, m_store->RenameTag(*editing, std::move(text)));
                    reset();
                    if (auto rebuild = weakReload.lock()) (*rebuild)();
                }
                catch (...) { report(::HudesNative::FriendlyError()); }
            };
            editor.Closing([this, editing, input, commit](auto const&, FlyoutBaseClosingEventArgs const& args)
            {
                if(std::wstring(input.Text())==*editing) return;
                auto choice=AskToSave(L"this tag");
                if(choice==IDYES) commit();
                args.Cancel(choice==IDCANCEL || (choice==IDYES && std::wstring(input.Text())!=*editing));
            });
            save.Click([commit](auto const&, auto const&) { commit(); });
            input.KeyDown([commit, reset](auto const&, KeyRoutedEventArgs const& args)
            {
                if (args.Key() == winrt::Windows::System::VirtualKey::Enter) { args.Handled(true); commit(); }
                if (args.Key() == winrt::Windows::System::VirtualKey::Escape) reset();
            });
            Button done;
            done.Content(box_value(L"Done"));
            done.HorizontalAlignment(HorizontalAlignment::Right);
            done.Click([weak = make_weak(editor)](auto const&, auto const&) { if (auto flyout = weak.get()) flyout.Hide(); });
            content.Children().Append(done);
            editor.Content(content);
            winrt::handle closed{ CreateEventW(nullptr, TRUE, FALSE, nullptr) };
            check_bool(static_cast<bool>(closed));
            auto dismissed = editor.Closed(auto_revoke, [event = closed.get()](auto const&, auto const&) { SetEvent(event); });
            auto windowClosed = Closed(auto_revoke, [event = closed.get()](auto const&, auto const&) { SetEvent(event); });
            auto ui = apartment_context();
            editor.ShowAt(ManageTagsButton());
            co_await resume_on_signal(closed.get());
            co_await ui;
            if (m_windowClosed) co_return;
            LoadTags(selected);
            {
                auto loading = std::exchange(m_loadingControls, true);
                auto restore = wil::scope_exit([&] { m_loadingControls = loading; });
                LoadSearchFilters();
                SetComboText(SearchTagFilter(), searchTag);
            }
            UpdateTagFilterButton();
            RefreshFiles();
            RefreshSearch();
        }
        catch (...) { ShowMessage(L"Tags", ::HudesNative::FriendlyError()); }
    }

    fire_and_forget MainWindow::ReminderDialog()
    {
        auto lifetime = get_strong();
        try
        {
            auto paths = SelectedPaths();
            if (paths.empty() || std::ranges::any_of(paths, [&](auto const& path) { return !CanManage(path); })) co_return;
            ContentDialog dialog;
            dialog.XamlRoot(RootGrid().XamlRoot());
            dialog.Title(box_value(L"Set reminder"));
            dialog.PrimaryButtonText(L"Set reminder");
            dialog.CloseButtonText(L"Cancel");
            dialog.DefaultButton(ContentDialogButton::Primary);
            StackPanel content;
            content.Spacing(10);
            content.Children().Append(MakeText(L"Date", 10, winrt::Windows::UI::Text::FontWeights::SemiBold()));
            CalendarView calendar;
            calendar.SelectionMode(CalendarViewSelectionMode::Single);
            calendar.IsTodayHighlighted(true);
            calendar.MaxHeight(330);
            calendar.HorizontalAlignment(HorizontalAlignment::Center);
            calendar.SelectedDates().Append(winrt::clock::now());
            content.Children().Append(calendar);
            Grid timeGrid;
            timeGrid.ColumnSpacing(8);
            ColumnDefinition hourColumn; hourColumn.Width({ 1, GridUnitType::Star }); timeGrid.ColumnDefinitions().Append(hourColumn);
            ColumnDefinition minuteColumn; minuteColumn.Width({ 1, GridUnitType::Star }); timeGrid.ColumnDefinitions().Append(minuteColumn);
            SYSTEMTIME now{}; GetLocalTime(&now);
            TextBox hour; hour.Header(box_value(L"Hour (0–23)")); hour.Text(std::to_wstring(now.wHour)); timeGrid.Children().Append(hour);
            TextBox minute; minute.Header(box_value(L"Minute (0–59)")); minute.Text(std::to_wstring(now.wMinute)); Grid::SetColumn(minute, 1); timeGrid.Children().Append(minute);
            content.Children().Append(timeGrid);
            dialog.Content(content);
            if (co_await dialog.ShowAsync() != ContentDialogResult::Primary) co_return;
            if (!calendar.SelectedDates().Size()) { ShowMessage(L"Reminder", L"Choose a date."); co_return; }
            std::size_t usedHour{}, usedMinute{};
            auto selectedHour = std::stoi(std::wstring(hour.Text()), &usedHour);
            auto selectedMinute = std::stoi(std::wstring(minute.Text()), &usedMinute);
            if (usedHour != hour.Text().size() || usedMinute != minute.Text().size() || selectedHour < 0 || selectedHour > 23 || selectedMinute < 0 || selectedMinute > 59)
            {
                ShowMessage(L"Reminder", L"Enter an hour from 0 to 23 and a minute from 0 to 59.");
                co_return;
            }
            auto chosen = winrt::clock::to_time_t(calendar.SelectedDates().GetAt(0));
            tm local{};
            localtime_s(&local, &chosen);
            SYSTEMTIME system{ static_cast<WORD>(local.tm_year + 1900), static_cast<WORD>(local.tm_mon + 1), 0,
                static_cast<WORD>(local.tm_mday), static_cast<WORD>(selectedHour), static_cast<WORD>(selectedMinute), 0, 0 };
            m_store->SetReminder(paths, ::HudesNative::LocalDateTimeText(system));
            RefreshFiles();
            StatusBarText().Text(L"Reminder saved");
        }
        catch (...) { ShowMessage(L"Reminder", ::HudesNative::FriendlyError()); }
    }

    fire_and_forget MainWindow::MetadataDialog(std::filesystem::path path)
    {
        auto lifetime = get_strong();
        try
        {
        auto ui = apartment_context();
        std::vector<::HudesNative::MetadataEntry> entries;
        std::exception_ptr readError;
        co_await resume_background();
        try { entries = ::HudesNative::ReadMetadata(path); }
        catch (...) { readError = std::current_exception(); }
        co_await ui;
        if (readError)
        {
            try { std::rethrow_exception(readError); }
            catch (...) { ShowMessage(L"File metadata", ::HudesNative::FriendlyError()); }
            co_return;
        }
        ContentDialog dialog; dialog.XamlRoot(RootGrid().XamlRoot()); dialog.Title(box_value(L"File metadata · " + path.filename().wstring())); dialog.PrimaryButtonText(L"Save changes"); dialog.CloseButtonText(L"Close"); dialog.DefaultButton(ContentDialogButton::Primary);
        ScrollViewer scroll; scroll.MaxHeight(570); scroll.MinWidth(720);
        StackPanel content; content.Spacing(5);
        std::vector<std::pair<size_t, TextBox>> editors;
        std::wstring category;
        for (size_t index = 0; index < entries.size(); ++index)
        {
            auto& entry = entries[index];
            if (entry.category != category)
            {
                category = entry.category;
                auto header = MakeText(category, 12, winrt::Windows::UI::Text::FontWeights::SemiBold()); header.Foreground(UiBrush(L"AccentBrush")); header.Margin({ 0, category == entries.front().category ? 0.0 : 14.0,0,4 }); content.Children().Append(header);
            }
            Grid row; row.ColumnSpacing(12); ColumnDefinition label; label.Width({ 190, GridUnitType::Pixel }); row.ColumnDefinitions().Append(label); ColumnDefinition value; value.Width({ 1, GridUnitType::Star }); row.ColumnDefinitions().Append(value);
            auto labelText = MakeText(entry.name, 10); labelText.Foreground(UiBrush(L"MutedBrush")); labelText.VerticalAlignment(VerticalAlignment::Center); row.Children().Append(labelText);
            if (entry.editable)
            {
                TextBox editor; editor.Text(entry.value); editor.FlowDirection(m_store->Settings().metadataRtl ? FlowDirection::RightToLeft : FlowDirection::LeftToRight); editor.TextAlignment(TextAlignment::Left); Grid::SetColumn(editor, 1); row.Children().Append(editor); editors.emplace_back(index, editor);
            }
            else
            {
                auto valueText = MakeText(entry.value, 10); valueText.TextWrapping(TextWrapping::Wrap); valueText.IsTextSelectionEnabled(true); valueText.VerticalAlignment(VerticalAlignment::Center); Grid::SetColumn(valueText, 1); row.Children().Append(valueText);
            }
            content.Children().Append(row);
        }
        scroll.Content(content); dialog.Content(scroll);
        if (co_await dialog.ShowAsync() != ContentDialogResult::Primary) co_return;
        for (auto const& [index, editor] : editors) entries[index].value = editor.Text().c_str();
        std::vector<std::wstring> failures;
        std::exception_ptr saveError;
        co_await resume_background();
        try { failures = ::HudesNative::SaveMetadata(path, entries); }
        catch (...) { saveError = std::current_exception(); }
        co_await ui;
        if (saveError)
        {
            try { std::rethrow_exception(saveError); }
            catch (...) { ShowMessage(L"File metadata", ::HudesNative::FriendlyError()); }
            co_return;
        }
        if (failures.empty()) StatusBarText().Text(L"File metadata saved");
        else { std::wstring message; for (auto const& failure : failures) message += failure + L"\r\n"; ShowMessage(L"Some properties were not saved", message); }
        RefreshFiles();
        }
        catch (...) { ShowMessage(L"File metadata", ::HudesNative::FriendlyError()); }
    }

    fire_and_forget MainWindow::CreateItemDialog(bool directory)
    {
        if (!m_ready || !IsWorkspaceLocation()) co_return;
        auto lifetime = get_strong();
        auto folder = m_currentFolder;
        auto title = directory ? L"New folder" : L"New file";
        try
        {
            auto const& settings = m_store->Settings();
            auto const& presets = settings.fileExtensions;
            auto defaultPreset = std::ranges::find_if(presets, [](auto const& preset) { return EqualText(preset.extension, L".txt"); });
            if (defaultPreset == presets.end() && !presets.empty()) defaultPreset = presets.begin();
            auto extension = defaultPreset == presets.end() ? std::wstring{} : defaultPreset->extension;
            auto suggestion = directory ? ::HudesNative::UniqueDestination(folder / L"New folder").filename()
                : std::filesystem::path(::HudesNative::NextFileName(folder, settings.newFileBase, extension));
            auto selection = static_cast<int32_t>(suggestion.native().size() - (directory ? 0 : extension.size()));
            ContentDialog dialog;
            dialog.XamlRoot(RootGrid().XamlRoot());
            EnableEditorClickAway(dialog);
            dialog.Title(box_value(title));
            dialog.PrimaryButtonText(L"Create");
            dialog.CloseButtonText(L"Cancel");
            dialog.DefaultButton(ContentDialogButton::Primary);
            StackPanel fields;
            fields.MinWidth(350);
            fields.Spacing(12);
            Border location;
            location.Background(UiBrush(L"CardBrush"));
            location.CornerRadius({ 9 });
            location.Padding({ 12, 9, 12, 9 });
            StackPanel details;
            details.Spacing(4);
            auto destinationLabel = MakeText(L"Create in", 11);
            destinationLabel.Foreground(UiBrush(L"MutedBrush"));
            auto destinationPath = MakeText(folder.wstring(), 13);
            destinationPath.TextTrimming(TextTrimming::CharacterEllipsis);
            ToolTipService::SetToolTip(destinationPath, box_value(folder.wstring()));
            details.Children().Append(destinationLabel);
            details.Children().Append(destinationPath);
            location.Child(details);
            fields.Children().Append(location);
            TextBox input;
            if (!directory)
            {
                ComboBox picker;
                picker.Header(box_value(L"File type"));
                picker.HorizontalAlignment(HorizontalAlignment::Stretch);
                ComboBoxItem custom;
                custom.Content(box_value(L"Custom extension"));
                custom.Tag(box_value(L""));
                picker.Items().Append(custom);
                picker.SelectedItem(custom);
                for (auto const& preset : presets)
                {
                    ComboBoxItem item;
                    item.Content(box_value(preset.name + L" (" + preset.extension + L")"));
                    item.Tag(box_value(preset.extension));
                    picker.Items().Append(item);
                    if (&preset == (defaultPreset == presets.end() ? nullptr : &*defaultPreset)) picker.SelectedItem(item);
                }
                picker.SelectionChanged([input, previous = extension](IInspectable const& sender, auto const&) mutable
                {
                    auto selected = sender.as<ComboBox>().SelectedItem().try_as<ComboBoxItem>();
                    if (!selected) return;
                    auto suffix = std::wstring(unbox_value_or<hstring>(selected.Tag(), L""));
                    if (suffix.empty()) return;
                    auto name = std::wstring(input.Text());
                    if (!previous.empty() && Lower(name).ends_with(Lower(previous))) name.resize(name.size() - previous.size());
                    else name = std::filesystem::path(name).stem().wstring();
                    input.Text(name + suffix);
                    previous = std::move(suffix);
                });
                fields.Children().Append(picker);
            }
            input.Header(box_value(directory ? L"Folder name" : L"File name"));
            input.Text(suggestion.wstring());
            fields.Children().Append(input);
            TextBlock error;
            error.TextWrapping(TextWrapping::Wrap);
            error.Foreground(UiBrush(L"DangerBrush"));
            fields.Children().Append(error);
            dialog.Content(fields);
            dialog.Opened([input, selection](auto const&, auto const&)
            {
                input.Focus(FocusState::Programmatic);
                input.Select(0, selection);
            });

            std::filesystem::path created;
            auto create = dialog.PrimaryButtonClick(auto_revoke, [&, input, error](auto const&, ContentDialogButtonClickEventArgs const& args)
            {
                try { created = ::HudesNative::CreateEmptyItem(folder, std::wstring(input.Text()), directory); }
                catch (...)
                {
                    args.Cancel(true);
                    error.Text(::HudesNative::FriendlyError());
                }
            });
            if (co_await dialog.ShowAsync() != ContentDialogResult::Primary) co_return;
            m_selectionToRestore = { created };
            RefreshFiles();
            RefreshSearch();
        }
        catch (...) { ShowMessage(title, ::HudesNative::FriendlyError()); }
    }

    fire_and_forget MainWindow::RenameDialog()
    {
        auto lifetime = get_strong();
        try
        {
            auto path = SelectedOne();
            if (!path || IsProtectedFolder(*path)) co_return;
            auto filename = path->filename();
            auto selection = static_cast<int32_t>((std::filesystem::is_directory(*path) ? filename : filename.stem()).native().size());
            ContentDialog dialog;
            dialog.XamlRoot(RootGrid().XamlRoot());
            dialog.Title(box_value(L"Rename"));
            dialog.PrimaryButtonText(L"Rename");
            dialog.CloseButtonText(L"Cancel");
            dialog.DefaultButton(ContentDialogButton::Primary);
            StackPanel fields;
            fields.Spacing(8);
            TextBox input;
            input.Text(filename.wstring());
            fields.Children().Append(input);
            TextBlock error;
            error.TextWrapping(TextWrapping::Wrap);
            error.Foreground(UiBrush(L"DangerBrush"));
            fields.Children().Append(error);
            dialog.Content(fields);
            dialog.Opened([input, selection](auto const&, auto const&)
            {
                input.Focus(FocusState::Programmatic);
                input.Select(0, selection);
            });
            std::filesystem::path destination;
            auto rename = dialog.PrimaryButtonClick(auto_revoke, [&, input, error](auto const&, ContentDialogButtonClickEventArgs const& args)
            {
                auto name = std::wstring(input.Text());
                if (name == filename.native()) return;
                try
                {
                    ::HudesNative::ValidateItemName(name);
                    auto target = path->parent_path() / name;
                    check_bool(MoveFileExW(path->c_str(), target.c_str(), 0));
                    destination = std::move(target);
                }
                catch (...)
                {
                    args.Cancel(true);
                    error.Text(::HudesNative::FriendlyError());
                }
            });
            if (co_await dialog.ShowAsync() != ContentDialogResult::Primary || destination.empty()) co_return;
            m_store->MovePath(*path, destination);
            m_selectionToRestore = { destination };
            RefreshFiles();
            RefreshSearch();
        }
        catch (...) { ShowMessage(L"Rename", ::HudesNative::FriendlyError()); }
    }

    fire_and_forget MainWindow::DeleteSelected()
    {
        auto lifetime = get_strong();
        try
        {
        auto paths = SelectedPaths(); if (paths.empty() || std::ranges::any_of(paths, [&](auto const& path) { return IsProtectedFolder(path); })) co_return;
        ContentDialog dialog; dialog.XamlRoot(RootGrid().XamlRoot()); dialog.Title(box_value(L"Delete selected items?")); dialog.Content(box_value(L"The selected items will be moved to the Recycle Bin.")); dialog.PrimaryButtonText(L"Delete"); dialog.CloseButtonText(L"Cancel"); dialog.DefaultButton(ContentDialogButton::Close);
        if (co_await dialog.ShowAsync() != ContentDialogResult::Primary) co_return;
        try
        {
            ::HudesNative::DeleteToRecycleBin(WindowHandle(), paths);
            m_store->DeletePaths(paths);
            RefreshFiles(); RefreshSearch();
        }
        catch (...) { ShowMessage(L"Delete", ::HudesNative::FriendlyError()); }
        }
        catch (...) { ShowMessage(L"Delete", ::HudesNative::FriendlyError()); }
    }

    fire_and_forget MainWindow::ArchiveSelected(bool zip)
    {
        auto lifetime = get_strong();
        try
        {
            auto paths = SelectedPaths();
            if (paths.empty() || std::ranges::any_of(paths, [&](auto const& path)
                { return !CanManage(path) || m_store->IsArchived(path); })) co_return;
            if (!zip)
            {
                m_store->SetArchived(paths, true);
                RefreshFiles(); RefreshSearch();
                co_return;
            }

            ContentDialog dialog;
            dialog.XamlRoot(RootGrid().XamlRoot());
            dialog.Title(box_value(L"Archive a ZIP copy"));
            dialog.PrimaryButtonText(L"Create ZIP copy");
            dialog.CloseButtonText(L"Cancel");
            TextBox name;
            name.Header(box_value(L"ZIP file name"));
            name.Text(L"Archive copy.zip");
            dialog.Content(name);
            if (co_await dialog.ShowAsync() != ContentDialogResult::Primary) co_return;
            auto filename = std::wstring(name.Text());
            if (!Lower(filename).ends_with(L".zip")) filename += L".zip";
            ::HudesNative::ValidateItemName(filename);
            auto output = ::HudesNative::UniqueDestination(m_desktop / filename);
            auto group = m_store->Get(paths.front()) ? m_store->Get(paths.front())->groupId : std::nullopt;
            for (auto const& path : paths)
            {
                auto note = m_store->Get(path);
                if ((note ? note->groupId : std::nullopt) != group) { group.reset(); break; }
            }

            auto ready = std::exchange(m_ready, false);
            MainInputContainer().IsEnabled(false);
            auto ui = apartment_context();
            std::exception_ptr error;
            co_await resume_background();
            try { ::HudesNative::CreateZip(paths, output); }
            catch (...) { error = std::current_exception(); }
            co_await ui;
            m_ready = ready;
            MainInputContainer().IsEnabled(true);
            if (error) std::rethrow_exception(error);
            std::array created{ output };
            if (group) m_store->SetGroup(created, group);
            m_store->SetArchived(created, true);
            RefreshFiles(); RefreshSearch();
        }
        catch (...) { ShowMessage(L"Archive", ::HudesNative::FriendlyError()); }
    }

    fire_and_forget MainWindow::UnarchiveSelected()
    {
        try
        {
            auto paths = SelectedPaths();
            if (!paths.empty() && std::ranges::all_of(paths, [&](auto const& path) { return CanManage(path) && m_store->IsArchived(path); }))
            {
                m_store->SetArchived(paths, false);
                RefreshFiles(); RefreshSearch();
            }
        }
        catch (...) { ShowMessage(L"Unarchive", ::HudesNative::FriendlyError()); }
        co_return;
    }

    fire_and_forget MainWindow::CreateZipDialog()
    {
        auto lifetime = get_strong();
        try
        {
        auto paths = SelectedPaths(); if (paths.empty() || std::ranges::any_of(paths, [&](auto const& path) { return !CanManage(path); })) co_return;
        SYSTEMTIME now{}; GetLocalTime(&now); wchar_t defaultName[40]{}; swprintf_s(defaultName, L"zip%04hu%02hu%02hu%02hu%02hu.zip", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute);
        auto output = ::HudesNative::PickSaveFile(WindowHandle(), L"Create ZIP", L"ZIP archive", L"*.zip", defaultName);
        if (!output) co_return;
        if (!Lower(output->wstring()).ends_with(L".zip")) *output += L".zip";
        StatusBarText().Text(L"Creating ZIP…");
        auto ui = apartment_context();
        std::vector<std::filesystem::path> outputs;
        std::exception_ptr error;
        co_await resume_background();
        try { outputs = ::HudesNative::CreateZip(paths, *output); }
        catch (...) { error = std::current_exception(); }
        co_await ui;
        if (error)
        {
            try { std::rethrow_exception(error); }
            catch (...) { ShowMessage(L"ZIP creation failed", ::HudesNative::FriendlyError()); }
            co_return;
        }
        StatusBarText().Text(L"ZIP file created");
        RefreshFiles();
        }
        catch (...) { ShowMessage(L"Create ZIP", ::HudesNative::FriendlyError()); }
    }

    fire_and_forget MainWindow::ExtractZipDialog()
    {
        auto lifetime = get_strong();
        try
        {
        auto path = SelectedOne(); if (!path || !CanManage(*path) || std::filesystem::is_directory(*path)) co_return;
        auto lower = Lower(path->wstring());
        if (!lower.ends_with(L".zip")) { ShowMessage(L"Extract ZIP", L"Select a standard .zip file."); co_return; }
        auto destination = ::HudesNative::PickFolder(WindowHandle(), L"Choose extraction folder", m_currentFolder); if (!destination) co_return;
        StatusBarText().Text(L"Extracting ZIP…");
        auto ui = apartment_context();
        std::exception_ptr error;
        co_await resume_background();
        try { ::HudesNative::ExtractZip(*path, *destination); }
        catch (...) { error = std::current_exception(); }
        co_await ui;
        if (error)
        {
            try { std::rethrow_exception(error); }
            catch (...) { ShowMessage(L"ZIP extraction failed", ::HudesNative::FriendlyError()); }
            co_return;
        }
        StatusBarText().Text(L"ZIP extracted"); RefreshFiles();
        }
        catch (...) { ShowMessage(L"ZIP extraction failed", ::HudesNative::FriendlyError()); }
    }

    fire_and_forget MainWindow::ChangePassword()
    {
        auto lifetime = get_strong();
        if (!m_store->VerifyPassword(CurrentPasswordBox().Password().c_str())) { ShowMessage(L"Change password", L"The current password is incorrect."); co_return; }
        auto password = std::wstring(NewPasswordBox().Password());
        if (password.size() < 4 || password != ConfirmPasswordBox().Password().c_str()) { ShowMessage(L"Change password", L"The new passwords must match and contain at least 4 characters."); co_return; }
        try
        {
            m_store->SetPassword(password); m_store->Save();
            CurrentPasswordBox().Password(L""); NewPasswordBox().Password(L""); ConfirmPasswordBox().Password(L"");
            StatusBarText().Text(L"Password changed");
        }
        catch (...) { ShowMessage(L"Change password", ::HudesNative::FriendlyError()); }
    }

    fire_and_forget MainWindow::TryWindowsHello()
    {
        auto lifetime = get_strong();
        auto currentSignIn = [this, generation = m_signInGeneration]
        {
            return generation == m_signInGeneration && LockOverlay().Visibility() == Visibility::Visible &&
                SignInPanel().Visibility() == Visibility::Visible;
        };
        if (!m_store || !m_store->Settings().useWindowsHello || m_helloPending || !currentSignIn()) co_return;
        m_helloPending = true;
        auto clearPending = wil::scope_exit([&] { m_helloPending = false; });
        try
        {
            auto verifier = get_activation_factory<UserConsentVerifier, ::IUserConsentVerifierInterop>();
            Windows::Foundation::IAsyncOperation<UserConsentVerificationResult> operation{ nullptr };
            hstring message{ L"Unlock Hudes" };
            check_hresult(verifier->RequestVerificationForWindowAsync(WindowHandle(), static_cast<HSTRING>(get_abi(message)),
                guid_of<decltype(operation)>(), put_abi(operation)));
            auto result = co_await operation;
            if (currentSignIn() && result != UserConsentVerificationResult::Verified &&
                result != UserConsentVerificationResult::Canceled)
                LoginErrorText().Text(L"Windows Hello is unavailable. Enter your password.");
            if (currentSignIn() && result == UserConsentVerificationResult::Verified) UnlockSucceeded();
        }
        catch (...) { if (currentSignIn()) LoginErrorText().Text(L"Windows Hello is unavailable. Enter your password."); }
    }

    std::optional<std::filesystem::path> MainWindow::DropFolder(DragEventArgs const& args)
    {
        if (!args.DataView().Contains(FileDragFormat) && !args.DataView().Contains(StandardDataFormats::StorageItems())) return std::nullopt;
        auto path = TaggedPathFromSource(args.OriginalSource().try_as<DependencyObject>());
        if (!path)
        {
            auto active = ActiveFileList();
            if (auto panel = active.ItemsPanelRoot())
                for (auto const& container : panel.Children())
                {
                    auto item = active.ItemFromContainer(container).try_as<FrameworkElement>();
                    if (!item) continue;
                    auto point = args.GetPosition(item);
                    if (point.X >= 0 && point.Y >= 0 && point.X < item.ActualWidth() && point.Y < item.ActualHeight())
                    {
                        path = std::filesystem::path(unbox_value_or<hstring>(item.Tag(), L"").c_str());
                        break;
                    }
                }
        }
        if (!path || !CanManage(*path)) return std::nullopt;
        std::error_code error;
        if (!std::filesystem::is_directory(*path, error)) return std::nullopt;
        return path;
    }

    fire_and_forget MainWindow::HandleDrop(DragEventArgs args)
    {
        auto target = DropFolder(args);
        if (!target && (!IsWorkspaceLocation() || args.DataView().Contains(FileDragFormat) ||
            !args.DataView().Contains(StandardDataFormats::StorageItems()))) co_return;
        bool internal = !m_fileDragPaths.empty() && args.DataView().Contains(FileDragFormat);
        auto folder = target.value_or(m_currentFolder);
        args.Handled(true);
        auto lifetime = get_strong();
        auto deferral = args.GetDeferral();
        auto complete = wil::scope_exit([&] { deferral.Complete(); });
        try
        {
            std::vector<std::filesystem::path> paths;
            if (internal) paths = m_fileDragPaths;
            else
            {
                auto items = co_await args.DataView().GetStorageItemsAsync();
                for (auto const& item : items) paths.emplace_back(item.Path().c_str());
            }
            TransferDroppedFiles(std::move(paths), folder, internal);
        }
        catch (...) { ShowMessage(L"Drop files", ::HudesNative::FriendlyError()); }
    }

    fire_and_forget MainWindow::TransferDroppedFiles(std::vector<std::filesystem::path> paths, std::filesystem::path folder, bool internal)
    {
        auto lifetime = get_strong();
        ++m_fileTransfers;
        std::exception_ptr error;
        try
        {
            if (internal && std::ranges::any_of(paths, [&](auto const& path)
            {
                return !CanManage(path) || ::HudesNative::SamePath(path, folder) || ::HudesNative::IsWithin(folder, path);
            })) throw hresult_invalid_argument(L"Choose a different folder inside the desktop.");
            auto sourcePaths = paths;
            if (internal) std::erase_if(paths, [&](auto const& path)
            {
                return std::ranges::any_of(sourcePaths, [&](auto const& parent)
                {
                    return !::HudesNative::SamePath(parent, path) && ::HudesNative::IsWithin(path, parent);
                });
            });
            std::vector<std::pair<std::filesystem::path, std::filesystem::path>> moved;
            auto ui = apartment_context(); co_await resume_background();
            try
            {
                for (auto const& path : paths)
                {
                    if (internal && ::HudesNative::SamePath(path.parent_path(), folder)) continue;
                    auto destination = ::HudesNative::CopyToFolder(path, folder, internal);
                    if (internal) moved.emplace_back(path, std::move(destination));
                }
            }
            catch (...) { error = std::current_exception(); }
            co_await ui;
            if (!moved.empty()) m_store->MovePaths(moved);
        }
        catch (...) { error = std::current_exception(); }
        --m_fileTransfers;
        RefreshFiles();
        RefreshSearch();
        if (error)
        {
            try { std::rethrow_exception(error); }
            catch (...) { ShowMessage(L"Drop files", ::HudesNative::FriendlyError()); }
        }
    }

    void MainWindow::Lock_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (!m_ready || !m_store->Settings().passwordRequired) return;
        m_watcher.Stop();
        m_ready = false;
        EndTerminalSession();
        m_editing = false;
        SetMetadataEditing(false);
        LoginPasswordBox().Password(L"");
        LoginErrorText().Text(L"");
        MainInputContainer().IsEnabled(false);
        SignInPanel().Visibility(Visibility::Collapsed);
        ManualLockPanel().Visibility(Visibility::Visible);
        LockOverlay().Visibility(Visibility::Visible);
        MainInputContainer().Visibility(Visibility::Collapsed);
        CompactWindow(true);
        ResumeLoginButton().Focus(FocusState::Programmatic);
    }

    void MainWindow::ResumeLogin_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (LockOverlay().Visibility() == Visibility::Visible && ManualLockPanel().Visibility() == Visibility::Visible)
            ShowSignIn();
    }

    void MainWindow::ShowSignIn()
    {
        ++m_signInGeneration;
        HelloButton().Visibility(m_store->Settings().useWindowsHello ? Visibility::Visible : Visibility::Collapsed);
        ManualLockPanel().Visibility(Visibility::Collapsed);
        SignInPanel().Visibility(Visibility::Visible);
        LockOverlay().Visibility(Visibility::Visible);
        LoginPasswordBox().Focus(FocusState::Programmatic);
        if (m_store->Settings().useWindowsHello) TryWindowsHello();
    }

    void MainWindow::HomeTab_Click(IInspectable const&, RoutedEventArgs const&)
    {
        ShowTab(HomeView(), HomeTabButton());
    }

    void MainWindow::SearchTab_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if(NoteForm().Visibility()==Visibility::Visible) return;
        RefreshSearch();
        ShowTab(SearchView(), SearchTabButton());
        SearchBox().Focus(FocusState::Programmatic);
    }

    void MainWindow::SettingsTab_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if(NoteForm().Visibility()==Visibility::Visible) return;
        EnsureFontsLoaded();
        ShowTab(SettingsView(), SettingsTabButton());
    }

    void MainWindow::ThemeToggle_Click(IInspectable const&, RoutedEventArgs const&)
    {
        try
        {
        auto& settings = m_store->Settings();
        settings.themeMode = EqualText(settings.themeMode, L"Dark") ? L"Light" : L"Dark";
        m_store->Save();
        ApplyTheme();
        ApplyTypography();
        TerminalStyle();
        ApplyTextDirection();
        ThemeToggleButton().Content(box_value(EqualText(settings.themeMode, L"Dark") ? L"☀" : L"☾"));
        auto selected = HomeView().Visibility() == Visibility::Visible ? HomeTabButton() :
            SearchView().Visibility() == Visibility::Visible ? SearchTabButton() : SettingsTabButton();
        HomeTabButton().Background(SolidColorBrush(winrt::Windows::UI::Color{}));
        SearchTabButton().Background(SolidColorBrush(winrt::Windows::UI::Color{}));
        SettingsTabButton().Background(SolidColorBrush(winrt::Windows::UI::Color{}));
        selected.Background(UiBrush(L"SelectedBrush"));
        selected.Foreground(UiBrush(L"InkBrush"));
        SelectModeButton().Background(UiBrush(m_selectMode ? L"SelectedBrush" : L"ControlBrush"));
        SelectModeButton().Foreground(UiBrush(L"InkBrush"));
        RefreshSearch();
        if (m_ready) RefreshFiles(false);
        }
        catch (...) { ShowMessage(L"Color mode", ::HudesNative::FriendlyError()); }
    }

    void MainWindow::HomeFolder_Click(IInspectable const&, RoutedEventArgs const&)
    {
        NavigateTo(m_startFolder);
    }

    void MainWindow::Up_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_currentFolder == ::HudesNative::ArchiveRoot()) { NavigateTo(m_desktop); return; }
        if (IsArchiveFolder(m_currentFolder)) { NavigateTo(::HudesNative::ArchiveRoot()); return; }
        if (auto note = m_store->Get(m_currentFolder); note && (note->archived || note->legacyArchiveSource) && CanManage(m_currentFolder))
        {
            NavigateTo(note->archived ? ArchiveLocation(*note) : m_desktop); return;
        }
        auto parent = m_currentFolder.parent_path();
        if (!parent.empty() && parent != m_currentFolder) NavigateTo(parent);
    }

    void MainWindow::Refresh_Click(IInspectable const&, RoutedEventArgs const&)
    {
        RefreshFiles(true);
    }

    void MainWindow::ArchiveFolder_Click(IInspectable const&, RoutedEventArgs const&)
    {
        NavigateTo(::HudesNative::ArchiveRoot());
    }

    void MainWindow::NewItem_Click(IInspectable const& sender, RoutedEventArgs const&)
    {
        if (!m_ready || !IsWorkspaceLocation()) return;
        MenuFlyout menu;
        PopulateNewItemMenu(menu.Items());
        menu.ShowAt(sender.as<FrameworkElement>());
    }

    void MainWindow::Direction_Click(IInspectable const&, RoutedEventArgs const&)
    {
        m_store->Settings().metadataRtl = !m_store->Settings().metadataRtl;
        m_store->Save();
        SetComboText(DefaultDirectionCombo(), m_store->Settings().metadataRtl ? L"RTL" : L"LTR");
        ApplyTextDirection();
        RefreshSearch();
    }

    void MainWindow::SaveMetadata_Click(IInspectable const&, RoutedEventArgs const&)
    {
        auto path = SelectedOne();
        if (!path || !CanManage(*path)) return;
        auto selected = NoteGroupCombo().SelectedItem().try_as<ComboBoxItem>();
        auto group = selected ? std::wstring(unbox_value_or<hstring>(selected.Tag(), L"").c_str()) : std::wstring{};
        UpdateRecords(0, {*path}, SideNoteBox().Text().c_str(), SelectedTags(), group.empty() ? std::nullopt : std::optional(group));
    }

    void MainWindow::CancelMetadata_Click(IInspectable const&, RoutedEventArgs const&)
    {
        SetMetadataEditing(false);
        ShowSelectedMetadata();
    }

    void MainWindow::EditNote_Click(IInspectable const&, RoutedEventArgs const&) { BeginMetadataEdit(); }

    void MainWindow::CreateGroup_Click(IInspectable const&, RoutedEventArgs const&) { CreateGroupDialog(); }
    void MainWindow::CreateTags_Click(IInspectable const&, RoutedEventArgs const&) { CreateTagsDialog(); }
    void MainWindow::TagPicker_Click(IInspectable const&, RoutedEventArgs const&) { UpdateTagSummary(); }
    void MainWindow::TagFilter_Click(IInspectable const&, RoutedEventArgs const&) { SelectTagFilters(); }

    void MainWindow::ClearFileFilters_Click(IInspectable const&, RoutedEventArgs const&)
    {
        {
            auto previousLoading = std::exchange(m_loadingControls, true);
            auto restoreLoading = wil::scope_exit([&] { m_loadingControls = previousLoading; });
            m_noteFilter.clear(); RenderNoteCards();
            FileNameFilterBox().Text(L"");
            HomeGroupFind().Text(L"");
            GroupFilterCombo().SelectedIndex(0);
            StatusFilterCombo().SelectedIndex(0);
        }
        m_tagFilters.clear();
        UpdateTagFilterButton();
        RefreshFiles();
    }

    void MainWindow::GroupFilter_Changed(IInspectable const&, SelectionChangedEventArgs const&)
    {
        if (m_ready && !m_loadingControls) RefreshFiles();
    }

    void MainWindow::StatusFilter_Changed(IInspectable const&, SelectionChangedEventArgs const&)
    {
        if (m_ready && !m_loadingControls) RefreshFiles();
    }

    void MainWindow::Sort_Changed(IInspectable const&, SelectionChangedEventArgs const&)
    {
        if (!m_ready || m_loadingControls || SortCombo().SelectedIndex() < 0) return;
        m_store->Settings().sortMethod = ComboText(SortCombo());
        m_store->Save();
        RefreshFiles(true);
    }

    void MainWindow::View_Changed(IInspectable const&, SelectionChangedEventArgs const&)
    {
        if (!m_ready || m_loadingControls || ViewCombo().SelectedIndex() < 0) return;
        m_store->Settings().viewMode = ComboText(ViewCombo());
        m_store->Save();
        RefreshFiles(true);
    }

    void MainWindow::FileNameFilter_Changed(IInspectable const&, TextChangedEventArgs const&)
    {
        if (m_ready && !m_loadingControls) RefreshFiles();
    }

    void MainWindow::Highlight_Changed(IInspectable const&, RoutedEventArgs const&)
    {
        auto checked = HighlightCheck().IsChecked();
        m_store->Settings().highlightGroupColors = checked && checked.Value();
        m_store->Save();
        if (m_ready) RefreshFiles();
    }

    void MainWindow::SortDirection_Click(IInspectable const&, RoutedEventArgs const&)
    {
        auto& settings = m_store->Settings();
        settings.sortDescending = !settings.sortDescending;
        SortDirectionButton().Content(box_value(settings.sortDescending ? L"↓" : L"↑"));
        m_store->Save();
        RefreshFiles(true);
    }

    void MainWindow::SelectMode_Click(IInspectable const&, RoutedEventArgs const&)
    {
        auto checked = SelectModeButton().IsChecked();
        m_selectMode = checked && checked.Value();
        auto mode = m_selectMode ? ListViewSelectionMode::Multiple : ListViewSelectionMode::Extended;
        FileGrid().SelectionMode(mode);
        FileDetails().SelectionMode(mode);
        SelectModeButton().Content(box_value(L"Select"));
        SelectModeButton().Background(UiBrush(m_selectMode ? L"SelectedBrush" : L"ControlBrush"));
        SelectModeButton().Foreground(UiBrush(L"InkBrush"));
        ShowSelectedMetadata();
    }

    void MainWindow::SelectGroups_Click(IInspectable const&, RoutedEventArgs const&) { SelectByDefinitions(true); }
    void MainWindow::SelectTags_Click(IInspectable const&, RoutedEventArgs const&) { SelectByDefinitions(false); }

    void MainWindow::CloseDefinitionPanel()
    {
        DefinitionPanel().Visibility(Visibility::Collapsed);
        DefinitionPanel().Child(nullptr);
    }

    void MainWindow::ShowDefinitionChoices(std::wstring const& title, std::wstring const& placeholder,
        std::wstring const& action, std::vector<std::pair<std::wstring, std::wstring>> const& definitions,
        std::vector<std::wstring> const& selected, std::function<void(std::vector<std::wstring>)> apply, bool single)
    {
        StackPanel content;
        content.Spacing(9);
        content.Children().Append(MakeText(title, 14, winrt::Windows::UI::Text::FontWeights::SemiBold()));
        TextBox find;
        find.PlaceholderText(placeholder);
        content.Children().Append(find);

        StackPanel choices;
        choices.Spacing(3);
        std::vector<ToggleButton> checks;
        for (auto const& [label, key] : definitions)
        {
            auto check = single ? RadioButton{}.as<ToggleButton>() : CheckBox{}.as<ToggleButton>();
            check.Content(box_value(label));
            check.Tag(box_value(key));
            check.IsChecked(std::ranges::any_of(selected, [&](auto const& value) { return EqualText(value, key); }));
            choices.Children().Append(check);
            checks.push_back(check);
        }
        auto empty = MakeText(L"No matching choices", 12);
        empty.Foreground(UiBrush(L"MutedBrush"));
        empty.Visibility(checks.empty() ? Visibility::Visible : Visibility::Collapsed);
        choices.Children().Append(empty);
        find.TextChanged([checks, empty](IInspectable const& sender, auto const&)
        {
            auto query = Lower(std::wstring(sender.as<TextBox>().Text()));
            bool shown = false;
            for (auto const& check : checks)
            {
                auto matches = Lower(std::wstring(unbox_value_or<hstring>(check.Content(), L""))).find(query) != std::wstring::npos;
                check.Visibility(matches ? Visibility::Visible : Visibility::Collapsed);
                shown = shown || matches;
            }
            empty.Visibility(shown ? Visibility::Collapsed : Visibility::Visible);
        });
        ScrollViewer scroll;
        scroll.MaxHeight(155);
        scroll.Content(choices);
        content.Children().Append(scroll);

        StackPanel buttons;
        buttons.Orientation(Orientation::Horizontal);
        buttons.Spacing(8);
        Button accept;
        accept.Content(box_value(action));
        accept.Background(UiBrush(L"SelectedBrush"));
        accept.Click([weak = get_weak(), checks, apply = std::move(apply)](auto const&, auto const&)
        {
            if (auto self = weak.get())
            {
                std::vector<std::wstring> values;
                for (auto const& check : checks)
                    if (check.IsChecked() && check.IsChecked().Value())
                        values.emplace_back(unbox_value_or<hstring>(check.Tag(), L""));
                self->CloseDefinitionPanel();
                apply(std::move(values));
            }
        });
        buttons.Children().Append(accept);
        Button cancel;
        cancel.Content(box_value(L"Cancel"));
        cancel.Click([weak = get_weak()](auto const&, auto const&) { if (auto self = weak.get()) self->CloseDefinitionPanel(); });
        buttons.Children().Append(cancel);
        if (title == L"Filter by tags")
        {
            Button clear;
            clear.Content(box_value(L"Clear tag filter"));
            clear.Click([weak = get_weak()](auto const&, auto const&)
            {
                if (auto self = weak.get())
                {
                    self->CloseDefinitionPanel();
                    self->m_tagFilters.clear();
                    self->UpdateTagFilterButton();
                    self->RefreshFiles();
                }
            });
            buttons.Children().Append(clear);
        }
        content.Children().Append(buttons);
        DefinitionPanel().Child(content);
        DefinitionPanel().Visibility(Visibility::Visible);
        find.Focus(FocusState::Programmatic);
    }

    void MainWindow::SelectTagFilters()
    {
        std::vector<std::pair<std::wstring, std::wstring>> definitions;
        for (auto const& tag : m_store->Tags()) definitions.emplace_back(tag, tag);
        ShowDefinitionChoices(L"Filter by tags", L"Find a tag", L"Apply filter", definitions, m_tagFilters,
            [weak = get_weak()](auto values)
            {
                if (auto self = weak.get())
                {
                    self->m_tagFilters = std::move(values);
                    self->UpdateTagFilterButton();
                    self->RefreshFiles();
                }
            });
    }

    void MainWindow::SelectByDefinitions(bool groups)
    {
        std::vector<std::pair<std::wstring, std::wstring>> definitions;
        if (groups)
            for (auto const& group : m_store->Groups()) definitions.emplace_back(group.name, group.id);
        else
            for (auto const& tag : m_store->Tags()) definitions.emplace_back(tag, tag);
        ShowDefinitionChoices(groups ? L"Select files by group" : L"Select files by tag",
            groups ? L"Find a group" : L"Find a tag", L"Select matching files", definitions, {},
            [weak = get_weak(), groups](auto values)
            {
                if (auto self = weak.get()) self->SelectMatchingDefinitions(groups, values);
            });
    }

    void MainWindow::SelectMatchingDefinitions(bool groups, std::vector<std::wstring> const& selectedDefinitions)
    {
        try
        {
            auto active = ActiveFileList();
            active.SelectedItems().Clear();
            m_selectionToRestore.clear();
            if (selectedDefinitions.empty())
            {
                m_lastSelectedPath.reset();
                ShowSelectedMetadata();
                UpdateAddress();
                return;
            }
            for (auto const& file : m_items)
            {
                auto note = m_store->Get(file.path);
                bool matches{};
                if (groups)
                {
                    matches = note && note->groupId && std::ranges::any_of(selectedDefinitions,
                        [&](auto const& id) { return EqualText(id, *note->groupId); });
                }
                else if (note)
                {
                    matches = std::ranges::any_of(note->tags, [&](auto const& tag)
                    {
                        return std::ranges::any_of(selectedDefinitions, [&](auto const& selected) { return EqualText(tag, selected); });
                    });
                }
                if (matches) m_selectionToRestore.push_back(file.path);
            }
            for (auto const& raw : active.Items())
            {
                if (auto element = raw.try_as<FrameworkElement>())
                {
                    auto path = std::filesystem::path(unbox_value_or<hstring>(element.Tag(), L"").c_str());
                    if (std::ranges::any_of(m_selectionToRestore, [&](auto const& selected) { return ::HudesNative::SamePath(path, selected); }))
                        active.SelectedItems().Append(raw);
                }
            }
            if (!m_selectionToRestore.empty()) m_lastSelectedPath = m_selectionToRestore.back();
            else m_lastSelectedPath.reset();
            if (m_buildIndex >= m_items.size()) m_selectionToRestore.clear();
            ShowSelectedMetadata();
            UpdateAddress();
        }
        catch (...) { ShowMessage(groups ? L"Group selecting" : L"Tag selecting", ::HudesNative::FriendlyError()); }
    }

    void MainWindow::FileList_SelectionChanged(IInspectable const& sender, SelectionChangedEventArgs const& args)
    {
        auto list = sender.try_as<ListViewBase>();
        if (m_marqueeActive || m_resolvingMetadata || !list || list != ActiveFileList()) return;
        if(m_editing && !ResolveMetadataDraft())
        {
            m_resolvingMetadata=true;
            list.SelectedItems().Clear();
            for(auto const& raw:list.Items())
                if(auto el=raw.try_as<FrameworkElement>();el && unbox_value_or<hstring>(el.Tag(),L"")==m_metadataPath.wstring())
                    {list.SelectedItems().Append(raw);break;}
            m_resolvingMetadata=false;
            return;
        }
        if (args.AddedItems().Size())
        {
            if (auto element = args.AddedItems().GetAt(args.AddedItems().Size() - 1).try_as<FrameworkElement>())
            {
                auto value = unbox_value_or<hstring>(element.Tag(), L"");
                if (!value.empty()) m_lastSelectedPath = std::filesystem::path(value.c_str());
            }
        }
        if (!list.SelectedItems().Size()) m_lastSelectedPath.reset();
        m_editing = false;
        ShowSelectedMetadata();
        UpdateAddress();
    }

    void MainWindow::FileList_DoubleTapped(IInspectable const&, DoubleTappedRoutedEventArgs const& args)
    {
        auto path = TaggedPathFromSource(args.OriginalSource().try_as<DependencyObject>());
        if (!path)
        {
            ActiveFileList().SelectedItems().Clear();
            args.Handled(true);
            return;
        }
        EnsureContextSelection(*path);
        OpenSelected();
        args.Handled(true);
    }

    void MainWindow::FileList_DragItemsStarting(IInspectable const&, DragItemsStartingEventArgs const& args)
    {
        m_fileDragPaths.clear();
        std::vector<std::filesystem::path> paths;
        for (auto const& item : args.Items())
            if (auto element = item.try_as<FrameworkElement>())
                paths.emplace_back(unbox_value_or<hstring>(element.Tag(), L"").c_str());
        args.Cancel(!PrepareFileDrag(args.Data(), std::move(paths)));
    }

    bool MainWindow::PrepareFileDrag(DataPackage const& data, std::vector<std::filesystem::path> paths)
    {
        if (paths.empty() || std::ranges::any_of(paths, [&](auto const& path) { return !CanManage(path); }))
        {
            return false;
        }
        auto items = single_threaded_vector<IStorageItem>();
        for (auto const& path : paths)
        {
            auto found = m_storageItems.find(path.wstring());
            if (found != m_storageItems.end()) items.Append(found->second);
        }
        if (items.Size() == paths.size()) data.SetStorageItems(items);
        m_fileDragPaths = std::move(paths);
        data.RequestedOperation(DataPackageOperation::Copy);
        data.SetData(FileDragFormat, box_value(L"workspace"));
        return true;
    }

    void MainWindow::FileList_DragItemsCompleted(ListViewBase const&, DragItemsCompletedEventArgs const&)
    {
        m_fileDragPaths.clear();
    }

    void MainWindow::FileView_DragOver(IInspectable const&, DragEventArgs const& args)
    {
        if (auto target = DropFolder(args))
        {
            bool internal = !m_fileDragPaths.empty() && args.DataView().Contains(FileDragFormat);
            args.AcceptedOperation(internal ? DataPackageOperation::Move : DataPackageOperation::Copy);
            args.DragUIOverride().Caption((internal ? L"Move to " : L"Copy to ") + target->filename().wstring());
            args.Handled(true);
        }
        else if (IsWorkspaceLocation() && !args.DataView().Contains(FileDragFormat) &&
            args.DataView().Contains(StandardDataFormats::StorageItems()))
        {
            args.AcceptedOperation(DataPackageOperation::Copy);
            args.DragUIOverride().Caption(L"Copy to " + m_currentFolder.filename().wstring());
            args.Handled(true);
        }
    }

    void MainWindow::FileView_Drop(IInspectable const&, DragEventArgs const& args) { HandleDrop(args); }

    void MainWindow::FileSurface_Tapped(IInspectable const&, TappedRoutedEventArgs const& args)
    {
        if (m_suppressSurfaceTap || TaggedPathFromSource(args.OriginalSource().try_as<DependencyObject>())) return;
        ActiveFileList().SelectedItems().Clear();
        m_selectionToRestore.clear();
        m_lastSelectedPath.reset();
        ShowSelectedMetadata();
        UpdateAddress();
    }

    void MainWindow::FileSurface_RightTapped(IInspectable const&, RightTappedRoutedEventArgs const& args)
    {
        if (m_suppressSurfaceTap || TaggedPathFromSource(args.OriginalSource().try_as<DependencyObject>())) return;
        ActiveFileList().SelectedItems().Clear();
        m_selectionToRestore.clear();
        MenuFlyout menu;
        if (IsWorkspaceLocation())
        {
            PopulateNewItemMenu(menu.Items());
            menu.Items().Append(MenuFlyoutSeparator());
            MenuFlyoutItem paste; paste.Text(L"Paste"); paste.KeyboardAcceleratorTextOverride(L"Ctrl+V");
            paste.Click([weak = get_weak()](auto const&, auto const&) { if (auto self = weak.get()) self->PasteFiles(); });
            menu.Items().Append(paste);
        }
        AppendViewMenu(menu,IsArchiveFolder(m_currentFolder)?m_desktop:m_currentFolder);
        MenuFlyoutItem refresh; refresh.Text(L"Refresh");
        refresh.Click([weak = get_weak()](auto const&, auto const&) { if (auto self = weak.get()) self->RefreshFiles(true); });
        menu.Items().Append(refresh);
        menu.ShowAt(FileSurface(), args.GetPosition(FileSurface()));
        args.Handled(true);
    }

    void MainWindow::CopyFileAddress_Click(IInspectable const&, RoutedEventArgs const&)
    {
        auto path = PreviewPath(); if (!path || IsProtectedFolder(*path)) return;
        try { DataPackage data; data.SetText(path->wstring()); Clipboard::SetContent(data); Clipboard::Flush(); StatusBarText().Text(L"File address copied"); }
        catch (...) { ShowMessage(L"Copy address", ::HudesNative::FriendlyError()); }
    }

    void MainWindow::CopyLocationAddress_Click(IInspectable const&, RoutedEventArgs const&)
    {
        auto path = PreviewPath(); if (!path || IsProtectedFolder(*path)) return;
        auto location = std::filesystem::is_directory(*path) ? *path : path->parent_path();
        try { DataPackage data; data.SetText(location.wstring()); Clipboard::SetContent(data); Clipboard::Flush(); StatusBarText().Text(L"Location address copied"); }
        catch (...) { ShowMessage(L"Copy address", ::HudesNative::FriendlyError()); }
    }
    void MainWindow::SearchBox_TextChanged(IInspectable const&, TextChangedEventArgs const&) { RefreshSearch(false); }

    void MainWindow::SearchResults_DoubleTapped(IInspectable const&, DoubleTappedRoutedEventArgs const& args)
    {
        try
        {
            if (auto item = SearchResults().SelectedItem().try_as<FrameworkElement>())
            {
                auto path = std::filesystem::path(unbox_value_or<hstring>(item.Tag(), L"").c_str());
                if (std::filesystem::exists(path)) ::HudesNative::OpenPath(path);
            }
        }
        catch (...) { ShowMessage(L"Open", ::HudesNative::FriendlyError()); }
        args.Handled(true);
    }

    void MainWindow::BrowseDesktopFolder_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (auto folder = ::HudesNative::PickFolder(WindowHandle(), L"Choose Desktop folder", DesktopFolderBox().Text().c_str()))
            DesktopFolderBox().Text(folder->wstring());
    }

    void MainWindow::BrowseStartFolder_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (auto folder = ::HudesNative::PickFolder(WindowHandle(), L"Choose starting folder", DefaultFolderBox().Text().c_str()))
            DefaultFolderBox().Text(folder->wstring());
    }

    void MainWindow::FileOptions_SizeChanged(IInspectable const&, SizeChangedEventArgs const& args)
    {
        ArrangeFileOptions(args.NewSize().Width);
    }

    void MainWindow::ArrangeFileOptions(double width)
    {
        if (m_arrangingFileOptions || width <= 0) return;
        m_arrangingFileOptions = true;

        auto first = FileOptionsRow1();
        auto second = FileOptionsRow2();
        auto third = FileOptionsRow3();
        std::array<UIElement, 11> options
        {
            DonateButton(), OptionSearch(), OptionGroup(), OptionTagFilter(), OptionStatus(), OptionSort(),
            OptionView(), OptionHighlight(), OptionGroupSelect(), OptionTagSelect(), OptionSelect()
        };

        first.Children().Clear();
        second.Children().Clear();
        third.Children().Clear();

        auto rows = FileOptionsRows().Children();
        while (rows.Size() > 4)
        {
            auto extraIndex = rows.Size() - 2;
            rows.GetAt(extraIndex).as<StackPanel>().Children().Clear();
            rows.RemoveAt(extraIndex);
        }
        std::vector<StackPanel> available{ first, second, third };
        size_t rowIndex{};
        double used{};
        for (auto const& option : options)
        {
            option.Measure({ 10000, 10000 });
            auto required = option.DesiredSize().Width;
            if (used > 0 && used + required > width) { ++rowIndex; used = 0; }
            if (rowIndex == available.size())
            {
                StackPanel extra;
                extra.Orientation(Orientation::Horizontal);
                extra.HorizontalAlignment(HorizontalAlignment::Center);
                rows.InsertAt(rows.Size() - 1, extra);
                available.push_back(extra);
            }
            available[rowIndex].Children().Append(option);
            used += required;
        }
        for (auto const& row : available)
            row.Visibility(row.Children().Size() ? Visibility::Visible : Visibility::Collapsed);
        m_arrangingFileOptions = false;
    }

    void MainWindow::SaveSettings_Click(IInspectable const&, RoutedEventArgs const&)
    {
        SaveSettings();
    }

    void MainWindow::ResetSettings_Click(IInspectable const&, RoutedEventArgs const&)
    {
        ResetSettings();
    }

    void MainWindow::SettingsSavedDismiss_Click(IInspectable const&, RoutedEventArgs const&)
    {
        SettingsSavedFlyout().Hide();
    }

    void MainWindow::ShowSettingsSavedPopup(std::wstring const& message)
    {
        SettingsSavedText().Text(message);
        FlyoutBase::ShowAttachedFlyout(SettingsSaveArea());
    }

    fire_and_forget MainWindow::SaveSettings()
    {
        auto lifetime = get_strong();
        try
        {
            auto parseNumber = [](hstring const& text, double minimum, double maximum)
            {
                std::size_t used{};
                auto value = std::stod(std::wstring(text), &used);
                if (!std::isfinite(value) || used != text.size() || value < minimum || value > maximum) throw std::invalid_argument("range");
                return value;
            };
            auto desktop = std::filesystem::path(DesktopFolderBox().Text().c_str());
            auto start = std::filesystem::path(DefaultFolderBox().Text().c_str());
            if (!std::filesystem::is_directory(desktop) || !std::filesystem::is_directory(start))
                throw hresult_invalid_argument(L"Both selected folders must exist.");

            auto settings = m_store->Settings();
            auto previousPasswordRequired = settings.passwordRequired;
            auto passwordChecked = PasswordRequiredCheck().IsChecked();
            auto passwordRequired = passwordChecked && passwordChecked.Value();

            if (previousPasswordRequired && !passwordRequired)
            {
                ContentDialog confirmation;
                confirmation.XamlRoot(RootGrid().XamlRoot());
                confirmation.Title(box_value(L"Confirm current password"));
                confirmation.PrimaryButtonText(L"Save and disable");
                confirmation.CloseButtonText(L"Cancel");
                confirmation.DefaultButton(ContentDialogButton::Primary);
                PasswordBox password;
                password.Header(box_value(L"Current password"));
                password.PasswordRevealMode(PasswordRevealMode::Peek);
                confirmation.Content(password);
                if (co_await confirmation.ShowAsync() != ContentDialogResult::Primary) co_return;
                if (!m_store->VerifyPassword(password.Password().c_str()))
                {
                    ShowMessage(L"Security", L"The current password is incorrect. No settings were changed.");
                    co_return;
                }
            }

            settings.desktopFolder = std::filesystem::absolute(desktop).lexically_normal();
            settings.desktopFolderConfirmed = true;
            settings.defaultFolder = std::filesystem::absolute(start).lexically_normal();
            settings.reminderWarningHours = parseNumber(ReminderHoursBox().Text(), 0.0, 87600.0);
            ReadAppearance(settings);
            settings.bashExecutable = BashExecutableBox().Text().c_str();
            ReadWorkspaceSettings(settings);
            settings.metadataRtl = EqualText(ComboText(DefaultDirectionCombo()), L"RTL");
            auto hello = WindowsHelloCheck().IsChecked();
            settings.terminalEnabled=ShowTerminalCheck().IsChecked().Value();
            if(!settings.terminalEnabled) settings.terminalPinned=false;
            if(auto shell=ShellChoice().SelectedItem().try_as<ComboBoxItem>()) settings.terminalShell=unbox_value_or<hstring>(shell.Tag(),L"cmd");
            settings.notesEnabled = ShowNotebookCheck().IsChecked().Value();
            if (!settings.notesEnabled) { m_noteFilter.clear(); m_notebookOpen = false; }
            settings.useWindowsHello = hello && hello.Value();
            settings.passwordRequired = passwordRequired;
            m_store->Settings() = settings;
            ApplyTheme();
            if(!settings.terminalEnabled) EndTerminalSession();
            UpdateTerminalPanel(); TerminalStyle(); UpdateNotebook(); RenderNoteCards();
            auto passwordWasEnabled = !previousPasswordRequired && passwordRequired;
            if (passwordWasEnabled) m_store->SetPassword(L"1111");
            m_store->Save();
            m_desktop = settings.desktopFolder;
            m_startFolder = settings.defaultFolder;
            ApplyTypography();
            LoadShortcuts();
            ApplyTextDirection();
            HelloButton().Visibility(settings.passwordRequired && settings.useWindowsHello ? Visibility::Visible : Visibility::Collapsed);
            if (m_ready) NavigateTo(m_startFolder, false);
            StatusBarText().Text(L"Settings saved");
            ShowSettingsSavedPopup(passwordWasEnabled
                ? L"Settings saved. Password protection is enabled and the password is set to 1111."
                : L"Settings saved");
        }
        catch (...) { ShowMessage(L"Settings", ::HudesNative::FriendlyError()); }
    }

    fire_and_forget MainWindow::ResetSettings()
    {
        auto lifetime = get_strong();
        try
        {
            ContentDialog dialog;
            dialog.XamlRoot(RootGrid().XamlRoot());
            dialog.Title(box_value(L"Reset settings?"));
            dialog.Content(box_value(L"This keeps the app desktop and starting folders. All other settings return to their defaults, password protection is enabled, and the password becomes 1111."));
            dialog.PrimaryButtonText(L"Reset");
            dialog.CloseButtonText(L"Cancel");
            dialog.DefaultButton(ContentDialogButton::Close);
            if (co_await dialog.ShowAsync() != ContentDialogResult::Primary) co_return;

            auto current = m_store->Settings();
            ::HudesNative::AppSettings defaults;
            defaults.desktopFolder = current.desktopFolder;
            defaults.desktopFolderConfirmed = current.desktopFolderConfirmed;
            defaults.defaultFolder = current.defaultFolder;
            m_store->Settings() = defaults;
            m_store->SetPassword(L"1111");
            m_store->Save();

            m_desktop = defaults.desktopFolder;
            m_startFolder = defaults.defaultFolder;
            m_fontsLoaded = false;
            m_tagFilters.clear();
            ApplyTheme();
            LoadSettingsControls();
            UpdateTagFilterButton();
            ApplyTextDirection();
            ApplyTypography();
            RefreshFiles();
            RefreshSearch();
            StatusBarText().Text(L"Settings reset to defaults");
            ShowMessage(L"Settings reset", L"Default settings were restored. Your app desktop and starting folders were kept. The password is 1111.");
        }
        catch (...) { ShowMessage(L"Settings", ::HudesNative::FriendlyError()); }
    }

    fire_and_forget MainWindow::PickTypographyColor(Button button)
    {
        auto lifetime = get_strong();
        try
        {
            ContentDialog dialog;
            dialog.XamlRoot(RootGrid().XamlRoot());
            dialog.Title(box_value(L"Choose color"));
            dialog.PrimaryButtonText(L"Use color");
            dialog.CloseButtonText(L"Cancel");
            ColorPicker picker;
            picker.IsAlphaEnabled(false);
            picker.IsColorSpectrumVisible(true);
            picker.IsColorSliderVisible(true);
            picker.IsMoreButtonVisible(true);
            picker.Color(ParseColor(unbox_value_or<hstring>(button.Tag(), L"#0B1727").c_str()));
            dialog.Content(picker);
            if (co_await dialog.ShowAsync() != ContentDialogResult::Primary) co_return;
            auto color = ColorHex(picker.Color());
            ShowSwatch(button,color);
        }
        catch (...) { ShowMessage(L"Text color", ::HudesNative::FriendlyError()); }
    }

    void MainWindow::ChangePassword_Click(IInspectable const&, RoutedEventArgs const&) { ChangePassword(); }

    void MainWindow::Unlock_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (LockOverlay().Visibility() != Visibility::Visible || SignInPanel().Visibility() != Visibility::Visible) return;
        if (m_store->VerifyPassword(LoginPasswordBox().Password().c_str())) UnlockSucceeded();
        else LoginErrorText().Text(L"Incorrect password.");
    }

    void MainWindow::Hello_Click(IInspectable const&, RoutedEventArgs const&) { TryWindowsHello(); }

    void MainWindow::LoginPassword_KeyDown(IInspectable const&, KeyRoutedEventArgs const& args)
    {
        if (args.Key() == winrt::Windows::System::VirtualKey::Enter)
        {
            Unlock_Click(nullptr, RoutedEventArgs{ nullptr });
            args.Handled(true);
        }
    }

    void MainWindow::FirstRunBrowse_Click(IInspectable const&, RoutedEventArgs const&)
    {
        auto folder = ::HudesNative::PickFolder(WindowHandle(), L"Choose your Desktop folder");
        if (!folder) return;
        FirstRunFolderBox().Text(folder->wstring());
        FirstRunContinueButton().IsEnabled(true);
    }

    void MainWindow::FirstRunContinue_Click(IInspectable const&, RoutedEventArgs const&)
    {
        auto folder = std::filesystem::path(FirstRunFolderBox().Text().c_str());
        if (!std::filesystem::is_directory(folder)) return;
        auto& settings = m_store->Settings();
        settings.desktopFolder = std::filesystem::absolute(folder).lexically_normal();
        settings.defaultFolder = settings.desktopFolder;
        settings.desktopFolderConfirmed = true;
        m_store->Save();
        FirstRunOverlay().Visibility(Visibility::Collapsed);
        m_desktop = settings.desktopFolder;
        m_startFolder = settings.defaultFolder;
        LoadSettingsControls();
        MainInputContainer().IsEnabled(true);
        m_ready = true;
        NavigateTo(m_startFolder, false);
        UpdateTerminalPanel();
        RefreshSearch();
    }

    void MainWindow::Root_KeyDown(IInspectable const&, KeyRoutedEventArgs const& args)
    {
        if (LockOverlay().Visibility() == Visibility::Visible || !m_ready) return;
        for (auto element = args.OriginalSource().try_as<DependencyObject>(); element; element = VisualTreeHelper::GetParent(element))
            if (element==TerminalPanel() || element==Notebook() || element.try_as<TextBox>() || element.try_as<PasswordBox>() || element.try_as<RichEditBox>()) return;
        auto control = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
        using Key = winrt::Windows::System::VirtualKey;
        if (control && args.Key() == Key::A)
        {
            auto list = ActiveFileList();
            list.SelectAll();
            if (m_currentFolder != ::HudesNative::ArchiveRoot())
                for (auto const& item : list.Items())
                {
                    auto element = item.try_as<FrameworkElement>();
                    uint32_t index{};
                    if (element && IsArchiveFolder(std::filesystem::path(unbox_value_or<hstring>(element.Tag(), L"").c_str()))
                        && list.SelectedItems().IndexOf(item, index)) list.SelectedItems().RemoveAt(index);
                }
        }
        else if (args.Key() == Key::Enter) OpenSelected();
        else if (args.Key() == Key::F5) RefreshFiles();
        else if (args.Key() == Key::Escape) ActiveFileList().SelectedItems().Clear();
        else if (args.Key() == Key::Back || ((GetKeyState(VK_MENU) & 0x8000) && args.Key() == Key::Up))
            Up_Click(nullptr, RoutedEventArgs{ nullptr });
        else if (control && args.Key() == winrt::Windows::System::VirtualKey::C) CopySelected(false);
        else if (control && args.Key() == winrt::Windows::System::VirtualKey::X) CopySelected(true);
        else if (control && args.Key() == winrt::Windows::System::VirtualKey::V) PasteFiles();
        else if (args.Key() == winrt::Windows::System::VirtualKey::Delete) DeleteSelected();
        else if (args.Key() == winrt::Windows::System::VirtualKey::F2) RenameDialog();
        else return;
        args.Handled(true);
    }

    void MainWindow::Splitter_PointerPressed(IInspectable const& sender, PointerRoutedEventArgs const& args)
    {
        auto grip=sender.as<FrameworkElement>();
        auto point=args.GetCurrentPoint(RootGrid());
        if (!m_store || m_splitterDragging || !point.Properties().IsLeftButtonPressed() || !grip.CapturePointer(args.Pointer())) return;
        m_resizePointer=args.Pointer().PointerId();
        m_splitterDragging=true;
        m_notesTimer.Stop(); m_detailsTimer.Stop(); m_terminalTimer.Stop();
        bool terminal=grip==TerminalGrip();
        m_splitterStartX=terminal ? point.Position().Y : point.Position().X;
        m_splitterStartWidth=terminal ? TerminalPanel().ActualHeight() : grip==NotesGrip() ? Notebook().ActualWidth() : DetailsPane().ActualWidth();
        args.Handled(true);
    }

    void MainWindow::Splitter_PointerMoved(IInspectable const& sender, PointerRoutedEventArgs const& args)
    {
        if (!m_splitterDragging || args.Pointer().PointerId()!=m_resizePointer) return;
        auto pos=args.GetCurrentPoint(RootGrid()).Position();
        auto& settings=m_store->Settings();
        auto grip=sender.as<FrameworkElement>();
        if (grip==TerminalGrip()) settings.terminalHeight=std::clamp(m_splitterStartWidth+m_splitterStartX-pos.Y,125.0,(std::max)(125.0,HomeView().ActualHeight()-135));
        else
        {
            auto delta=pos.X-m_splitterStartX;
            auto& width=grip==NotesGrip() ? settings.notesWidth : settings.detailsWidth;
            width=std::clamp(m_splitterStartWidth+(grip==NotesGrip() ? -delta : delta),180.0,(std::max)(180.0,HomeView().ActualWidth()*0.42));
        }
        UpdateNotebook(); args.Handled(true);
    }

    void MainWindow::Splitter_PointerReleased(IInspectable const& sender, PointerRoutedEventArgs const& args)
    {
        if (!m_splitterDragging || args.Pointer().PointerId()!=m_resizePointer) return;
        m_splitterDragging=false;
        sender.as<UIElement>().ReleasePointerCapture(args.Pointer());
        try {m_store->Save();} catch (...) {ShowMessage(L"Panel size",::HudesNative::FriendlyError());}
        args.Handled(true);
    }
}

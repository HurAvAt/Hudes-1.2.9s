#include "pch.h"
#include "MainWindow.xaml.h"
#include "DonationWallets.h"
#include <algorithm>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Peers.h>

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Media;
using namespace winrt::Microsoft::UI::Xaml::Media::Imaging;
using namespace ::HudesNative::Donations;

namespace
{
    TextBlock Invitation()
    {
        TextBlock label;
        label.Text(message);
        label.TextWrapping(TextWrapping::Wrap);
        label.FontSize(15);
        return label;
    }
}

namespace winrt::HudesNative::implementation
{
    void MainWindow::Donate_Click(IInspectable const&, RoutedEventArgs const&)
    {
        ShowDonation(false);
    }

    IAsyncAction MainWindow::ShowDonation(bool firstStart)
    {
        auto lifetime = get_strong();
        if (m_donationDialogOpen || m_windowClosed) co_return;
        m_donationDialogOpen = true;
        auto reset = wil::scope_exit([this] { m_donationDialogOpen = false; });
        try
        {
            if (firstStart)
            {
                auto marker = ::HudesNative::ExecutableFolder() / L".hudes-donate-welcome";
                if (GetFileAttributesW(marker.c_str()) != INVALID_FILE_ATTRIBUTES) co_return;

                ContentDialog welcome;
                welcome.XamlRoot(RootGrid().XamlRoot());
                welcome.RequestedTheme(RootGrid().RequestedTheme());
                welcome.Title(box_value(L"Support Hudes"));
                welcome.Content(Invitation());
                welcome.PrimaryButtonText(L"Donate");
                welcome.CloseButtonText(L"Maybe later");
                welcome.DefaultButton(ContentDialogButton::Close);
                auto result = co_await welcome.ShowAsync();
                if (m_windowClosed) co_return;
                wil::unique_hfile seen{CreateFileW(marker.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                    OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr)};
                if (result != ContentDialogResult::Primary) co_return;
            }

            ContentDialog dialog;
            dialog.XamlRoot(RootGrid().XamlRoot());
            dialog.RequestedTheme(RootGrid().RequestedTheme());
            dialog.Title(box_value(L"Donate to Hudes"));
            dialog.CloseButtonText(L"Close");

            StackPanel content;
            content.Spacing(12);
            content.Width(std::clamp(RootGrid().ActualWidth()-100.0, 180.0, 448.0));
            content.Children().Append(Invitation());
            ScrollViewer scroll;
            scroll.MaxHeight(std::clamp(RootGrid().ActualHeight()-220.0, 100.0, 465.0));
            scroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
            scroll.HorizontalScrollMode(ScrollMode::Disabled);
            StackPanel cards;
            cards.Spacing(6);
            TextBlock feedback;
            feedback.MinHeight(17);
            feedback.TextWrapping(TextWrapping::Wrap);
            feedback.FontSize(12);
            Automation::AutomationProperties::SetLiveSetting(feedback, Automation::Peers::AutomationLiveSetting::Polite);
            for (auto const& wallet : wallets)
            {
                Border card;
                card.Padding({11,8,11,8});
                card.CornerRadius({8});
                card.Background(UiBrush(L"CardBrush"));
                card.BorderBrush(UiBrush(L"BorderBrush"));
                card.BorderThickness({1});
                StackPanel fields;
                fields.Spacing(6);
                Grid header;
                ColumnDefinition logoColumn; logoColumn.Width(GridLengthHelper::Auto());
                ColumnDefinition nameColumn; nameColumn.Width({1,GridUnitType::Star});
                ColumnDefinition copyColumn; copyColumn.Width(GridLengthHelper::Auto());
                header.ColumnDefinitions().Append(logoColumn);
                header.ColumnDefinitions().Append(nameColumn);
                header.ColumnDefinitions().Append(copyColumn);
                Image icon;
                icon.Width(26); icon.Height(26);
                icon.Source(SvgImageSource(Uri(std::wstring(L"ms-appx:///Assets/Wallets/")+wallet.icon+L".svg")));
                header.Children().Append(icon);
                TextBlock network;
                network.Text(wallet.name);
                network.Margin({8,0,4,0});
                network.VerticalAlignment(VerticalAlignment::Center);
                network.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
                Grid::SetColumn(network,1); header.Children().Append(network);
                Button copy;
                copy.Content(box_value(L"Copy"));
                Automation::AutomationProperties::SetName(copy,std::wstring(L"Copy ")+wallet.name+L" address");
                copy.Padding({12,3,12,3});
                copy.MinHeight(28);
                copy.Click([owner=WindowHandle(), wallet, feedback](auto const&,auto const&)
                {
                    bool ok=::HudesNative::Donations::CopyAddress(owner,wallet);
                    feedback.Text(ok ? std::wstring(wallet.name)+L" address copied." : L"Clipboard is busy. Please try Copy again.");
                });
                Grid::SetColumn(copy,2); header.Children().Append(copy);
                fields.Children().Append(header);
                TextBlock address;
                address.Text(wallet.address);
                address.FontFamily(FontFamily(L"Consolas"));
                address.FontSize(13);
                address.TextWrapping(TextWrapping::Wrap);
                address.IsTextSelectionEnabled(true);
                address.FlowDirection(FlowDirection::LeftToRight);
                fields.Children().Append(address);
                card.Child(fields);
                cards.Children().Append(card);
            }
            scroll.Content(cards);
            content.Children().Append(scroll);
            content.Children().Append(feedback);
            dialog.Content(content);
            co_await dialog.ShowAsync();
        }
        catch (...)
        {
            if (!m_windowClosed) ShowMessage(L"Donations", ::HudesNative::FriendlyError());
        }
    }
}

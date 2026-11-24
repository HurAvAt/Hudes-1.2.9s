#include "pch.h"
#include "MainWindow.xaml.h"

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;

namespace winrt::HudesNative::implementation
{
    fire_and_forget MainWindow::InitializeDatabase()
    {
        if (m_databaseOpening || m_store) co_return;
        m_databaseOpening = true;
        auto lifetime = get_strong();
        try
        {
            hstring password;
            std::optional<hstring> migrationPassword;
            while (!m_store)
            {
                bool needsMigrationChoice{};
                try
                {
                    std::optional<std::wstring_view> migration;
                    if (migrationPassword) migration.emplace(migrationPassword->c_str(), migrationPassword->size());
                    m_store = std::make_unique<::HudesNative::AppStore>(std::filesystem::path{}, password.c_str(), migration);
                }
                catch (::HudesNative::DatabasePasswordRequired const&) {}
                catch (::HudesNative::DatabaseMigrationChoiceRequired const&) { needsMigrationChoice = true; }
                if (m_store) break;
                if (needsMigrationChoice)
                {
                    auto result = co_await PromptDatabasePassword(L"Convert the Hudes database",
                        L"This database is tied to this Windows account. Use a password of at least 8 characters to keep it encrypted and open it on any PC, or continue without encryption.", true, true);
                    if (!result) { Close(); co_return; }
                    migrationPassword = unbox_value<hstring>(result);
                    continue;
                }
                auto description = password.empty()
                    ? L"Enter the password set for this database."
                    : L"The password was incorrect or the database was damaged.";
                auto result = co_await PromptDatabasePassword(L"Open Hudes database", description, false);
                if (!result) { Close(); co_return; }
                password = unbox_value<hstring>(result);
            }

            ApplyTheme();
            co_await ShowDonation(true);
            if (m_windowClosed) co_return;
            if (!m_store->Settings().passwordRequired)
            {
                UnlockSucceeded();
                co_return;
            }
            ShowSignIn();
        }
        catch (...)
        {
            auto message = ::HudesNative::FriendlyError();
            MessageBoxW(WindowHandle(), message.c_str(), L"Could not open Hudes database", MB_OK | MB_ICONERROR);
            Close();
        }
    }

    IAsyncOperation<IInspectable> MainWindow::PromptDatabasePassword(
        std::wstring title, std::wstring description, bool newPassword, bool allowUnprotected)
    {
        auto lifetime = get_strong();
        ContentDialog dialog;
        dialog.XamlRoot(RootGrid().XamlRoot());
        dialog.Title(box_value(title));
        dialog.PrimaryButtonText(newPassword ? L"Use password" : L"Unlock database");
        dialog.CloseButtonText(L"Cancel");
        dialog.DefaultButton(ContentDialogButton::Primary);
        dialog.IsPrimaryButtonEnabled(false);
        if (allowUnprotected) dialog.SecondaryButtonText(L"Without password");

        StackPanel fields;
        fields.Spacing(10);
        TextBlock instructions;
        instructions.Text(description);
        instructions.TextWrapping(TextWrapping::Wrap);
        fields.Children().Append(instructions);
        PasswordBox password;
        password.Header(box_value(L"Database password"));
        password.MaxLength(1024);
        fields.Children().Append(password);
        PasswordBox confirmation{ nullptr };
        if (newPassword)
        {
            confirmation = PasswordBox();
            confirmation.Header(box_value(L"Confirm password"));
            confirmation.MaxLength(1024);
            fields.Children().Append(confirmation);
        }
        auto checkInput = [weakDialog = make_weak(dialog), password, confirmation](auto const&, auto const&)
        {
            if (auto dialog = weakDialog.get())
            {
                auto value = password.Password();
                dialog.IsPrimaryButtonEnabled(confirmation ? value.size() >= 8 && value == confirmation.Password() : !value.empty());
            }
        };
        auto onPassword = password.PasswordChanged(auto_revoke, checkInput);
        auto clearInputs = wil::scope_exit([&]
        {
            password.Password(L"");
            if (confirmation) confirmation.Password(L"");
        });
        PasswordBox::PasswordChanged_revoker onConfirmation;
        if (confirmation) onConfirmation = confirmation.PasswordChanged(auto_revoke, checkInput);
        dialog.Content(fields);
        auto result = co_await dialog.ShowAsync();
        if (result == ContentDialogResult::Primary) co_return box_value(password.Password());
        if (result == ContentDialogResult::Secondary) co_return box_value(hstring{});
        co_return nullptr;
    }

    void MainWindow::UpdateDatabaseProtection()
    {
        auto protectedDatabase = m_store->IsDatabaseProtected();
        DatabaseProtectionText().Text(protectedDatabase
            ? L"Password protected. You’ll enter this password when opening the database."
            : L"Password protection is off. Set a password to encrypt the database.");
        ProtectDatabaseButton().Content(box_value(protectedDatabase ? L"Change database password…" : L"Set database password…"));
        RemoveDatabasePasswordButton().Visibility(protectedDatabase ? Visibility::Visible : Visibility::Collapsed);
    }

    fire_and_forget MainWindow::ChangeDatabaseProtection(bool remove)
    {
        auto lifetime = get_strong();
        try
        {
            hstring password;
            if (remove)
            {
                ContentDialog dialog;
                dialog.XamlRoot(RootGrid().XamlRoot());
                dialog.Title(box_value(L"Remove database password?"));
                dialog.Content(box_value(L"The live database will be saved without encryption."));
                dialog.PrimaryButtonText(L"Remove password");
                dialog.CloseButtonText(L"Cancel");
                dialog.DefaultButton(ContentDialogButton::Close);
                if (co_await dialog.ShowAsync() != ContentDialogResult::Primary) co_return;
            }
            else
            {
                auto result = co_await PromptDatabasePassword(L"Protect the live database",
                    L"Use at least 8 characters. Keep this password so you can reopen the database on this or another PC.", true);
                if (!result) co_return;
                password = unbox_value<hstring>(result);
            }
            m_store->SetDatabasePassword(password.c_str());
            UpdateDatabaseProtection();
        }
        catch (...) { ShowMessage(L"Database protection", ::HudesNative::FriendlyError()); }
    }

    fire_and_forget MainWindow::BackupDatabase()
    {
        auto lifetime = get_strong();
        try
        {
            auto result = co_await PromptDatabasePassword(L"Protect this backup?",
                L"Use at least 8 characters to encrypt this backup. You’ll need the same password to restore it on any PC.", true, true);
            if (!result) co_return;
            auto path = ::HudesNative::PickSaveFile(WindowHandle(), L"Back up Hudes database", L"Hudes backup", L"*.hudesdb", L"hudes-backup.hudesdb");
            if (!path) co_return;
            auto password = unbox_value<hstring>(result);
            m_store->BackupTo(*path, password.c_str());
            ShowMessage(L"Database backup saved", path->wstring());
        }
        catch (...) { ShowMessage(L"Backup failed", ::HudesNative::FriendlyError()); }
    }

    fire_and_forget MainWindow::RestoreDatabase()
    {
        auto lifetime = get_strong();
        try
        {
            auto path = ::HudesNative::PickOpenFile(WindowHandle(), L"Restore Hudes database", L"Hudes database or older JSON backup", L"*.hudesdb;*.salifdb;*.json");
            if (!path) co_return;
            ContentDialog dialog;
            dialog.XamlRoot(RootGrid().XamlRoot());
            dialog.Title(box_value(L"Restore database?"));
            dialog.Content(box_value(L"This replaces the current database with the selected backup."));
            dialog.PrimaryButtonText(L"Restore");
            dialog.CloseButtonText(L"Cancel");
            dialog.DefaultButton(ContentDialogButton::Close);
            if (co_await dialog.ShowAsync() != ContentDialogResult::Primary) co_return;

            hstring password;
            while (true)
            {
                try { m_store->RestoreFrom(*path, password.c_str()); break; }
                catch (::HudesNative::DatabasePasswordRequired const&) {}
                auto description = password.empty()
                    ? L"Enter the password set when this backup was created."
                    : L"The password was incorrect or the backup was damaged.";
                auto result = co_await PromptDatabasePassword(L"Unlock database backup", description, false);
                if (!result) co_return;
                password = unbox_value<hstring>(result);
            }

            ApplyTheme();
            m_noteFilter.clear();
            m_noteDraft = {};
            m_notebookOpen = false;
            NoteForm().Visibility(Visibility::Collapsed);
            NewPanelNote().IsEnabled(true);
            m_fontsLoaded = false;
            LoadSettingsControls();
            LoadGroups();
            ApplyTextDirection();
            ApplyTypography();
            m_desktop = m_store->Settings().desktopFolder;
            m_startFolder = m_store->Settings().defaultFolder;
            NavigateTo(m_startFolder, false);
            RefreshSearch();
            ShowMessage(L"Database restored", L"Hudes loaded the selected backup.");
        }
        catch (...) { ShowMessage(L"Restore failed", ::HudesNative::FriendlyError()); }
    }

    void MainWindow::ProtectDatabase_Click(IInspectable const&, RoutedEventArgs const&) { ChangeDatabaseProtection(false); }
    void MainWindow::RemoveDatabasePassword_Click(IInspectable const&, RoutedEventArgs const&) { ChangeDatabaseProtection(true); }
    void MainWindow::BackupDatabase_Click(IInspectable const&, RoutedEventArgs const&) { BackupDatabase(); }
    void MainWindow::RestoreDatabase_Click(IInspectable const&, RoutedEventArgs const&) { RestoreDatabase(); }
}

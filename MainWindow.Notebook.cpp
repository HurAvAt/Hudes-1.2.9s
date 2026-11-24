#include "pch.h"
#include "MainWindow.xaml.h"
#include <algorithm>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <cwctype>

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;

namespace
{
    std::wstring Fold(std::wstring value)
    {
        std::ranges::transform(value, value.begin(), [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
        return value;
    }
}

namespace winrt::HudesNative::implementation
{
    void MainWindow::SetupNotebook()
    {
        m_detailsTimer=DispatcherTimer();
        m_detailsTimer.Interval(std::chrono::seconds(2));
        m_detailsTimer.Tick([this](auto const&,auto const&)
        {
            m_detailsTimer.Stop();
            if (m_store && !m_store->Settings().detailsPinned && !m_editing && !m_splitterDragging)
            { m_detailsOpen=false; UpdateNotebook(); }
        });
        DetailsPane().PointerEntered([this](auto const&,auto const&){m_detailsTimer.Stop();});
        DetailsPane().PointerExited([this](auto const&,auto const&){
            if(m_store && m_detailsOpen && !m_store->Settings().detailsPinned && !m_editing && !m_splitterDragging) m_detailsTimer.Start();
        });
        DetailsRail().Click([this](auto const&,auto const&){m_detailsOpen=true; UpdateNotebook();});
        PinDetails().Click([this](auto const&,auto const&){
            m_store->Settings().detailsPinned=PinDetails().IsChecked().Value(); m_detailsOpen=true;
            try {m_store->Save();} catch (...) {ShowMessage(L"File details",::HudesNative::FriendlyError());}
            UpdateNotebook();
        });
        m_notesTimer = DispatcherTimer();
        m_notesTimer.Interval(std::chrono::seconds(2));
        m_notesTimer.Tick([this](auto const&, auto const&)
        {
            m_notesTimer.Stop();
            if (!m_store || m_store->Settings().notesPinned || m_noteMenu || m_splitterDragging || NoteForm().Visibility()==Visibility::Visible) return;
            m_notebookOpen = false;
            UpdateNotebook();
        });
        Notebook().PointerEntered([this](auto const&, auto const&) { m_notesTimer.Stop(); });
        Notebook().PointerExited([this](auto const&, auto const&)
        {
            if (m_notebookOpen && m_store && !m_store->Settings().notesPinned && !m_noteMenu && NoteForm().Visibility()!=Visibility::Visible) m_notesTimer.Start();
        });
        NotesRail().Click([this](auto const&, auto const&) { m_notebookOpen = true; UpdateNotebook(); });
        PinNotebook().Click([this](auto const&, auto const&)
        {
            auto& settings = m_store->Settings();
            settings.notesPinned = PinNotebook().IsChecked().Value();
            m_notebookOpen = true;
            try { m_store->Save(); } catch (...) { ShowMessage(L"Notes", ::HudesNative::FriendlyError()); }
            UpdateNotebook();
        });
        NotebookSearch().TextChanged([this](auto const&, auto const&) { if (m_store) RenderNoteCards(); });
        NewPanelNote().Click([this](auto const&, auto const&) { EditPanelNote({}); });
        CancelPanelNote().Click([this](auto const&, auto const&)
        {
            FinishNoteDraft();
        });
        LinkSelected().Click([this](auto const&, auto const&)
        {
            auto count = m_noteDraft.files.size();
            for (auto const& path : SelectedPaths())
                if (CanManage(path) && std::ranges::none_of(m_noteDraft.files, [&](auto const& linked)
                    { return ::HudesNative::SamePath(linked, path); })) m_noteDraft.files.push_back(path);
            RenderDraftFiles();
            NoteError().Text(count == m_noteDraft.files.size() ? L"Select another workspace item to link." : L"");
        });
        CommitPanelNote().Click([this](auto const&, auto const&)
        {
            SaveNoteDraft();
        });
    }

    void MainWindow::FinishNoteDraft()
    {
        NoteForm().Visibility(Visibility::Collapsed);
        NewPanelNote().IsEnabled(true);
        m_noteDraft = {};
    }

    bool MainWindow::SaveNoteDraft()
    {
        try
        {
            m_noteDraft.title = NotebookTitle().Text();
            m_noteDraft.body = NotebookBody().Text();
            m_store->SavePanelNote(m_noteDraft);
            FinishNoteDraft();
            RenderNoteCards();
            if (!m_noteFilter.empty()) { RefreshFiles(); UpdateAddress(); }
            return true;
        }
        catch (...) { NoteError().Text(::HudesNative::FriendlyError()); return false; }
    }

    int MainWindow::AskToSave(wchar_t const* kind)
    {
        auto question=std::wstring(L"Save changes to ")+kind+L"?\n\nYes saves your changes. No discards them. Cancel keeps editing.";
        return MessageBoxW(WindowHandle(),question.c_str(),L"Unsaved changes",MB_YESNOCANCEL|MB_ICONQUESTION|MB_DEFBUTTON3);
    }

    void MainWindow::OutsideNotebook()
    {
        if(!m_store || !m_notebookOpen || m_noteMenu || m_splitterDragging || NoteForm().Visibility()==Visibility::Visible) return;
        if(!m_store->Settings().notesPinned) {m_notebookOpen=false; UpdateNotebook();}
    }

    void MainWindow::UpdateNotebook()
    {
        if (!m_store) return;
        auto const& settings = m_store->Settings();
        bool open = settings.notesEnabled && (m_notebookOpen || settings.notesPinned);
        double window=HomeView().ActualWidth();
        if (window<1) window=1200;
        double limit=(std::max)(180.0, window*0.42);
        double width=std::clamp(settings.notesWidth,180.0,limit);
        double details=std::clamp(settings.detailsWidth,180.0,limit);
        bool detailsVisible=settings.detailsPinned || m_detailsOpen || m_editing;
        DetailsPane().Visibility(detailsVisible ? Visibility::Visible : Visibility::Collapsed);
        DetailsRail().Visibility(detailsVisible ? Visibility::Collapsed : Visibility::Visible);
        DetailsPane().Width(details);
        SideColumn().Width({settings.detailsPinned ? details : 30.0,GridUnitType::Pixel});
        PinDetails().IsChecked(settings.detailsPinned);
        if (HomeView().ActualHeight()>0) TerminalPanel().Height(std::clamp(settings.terminalHeight,125.0,(std::max)(125.0,HomeView().ActualHeight()-135)));
        Notebook().Width(width);
        NotesColumn().Width({ settings.notesEnabled ? (settings.notesPinned ? width : 30.0) : 0.0, GridUnitType::Pixel });
        NotesRail().Visibility(settings.notesEnabled && !open ? Visibility::Visible : Visibility::Collapsed);
        Notebook().Visibility(open ? Visibility::Visible : Visibility::Collapsed);
        PinNotebook().IsChecked(settings.notesPinned);
        if (settings.notesPinned || !open) m_notesTimer.Stop();
    }

    void MainWindow::EditPanelNote(::HudesNative::PanelNote note)
    {
        m_notesTimer.Stop(); m_notebookOpen=true;
        m_noteDraft = std::move(note);
        NotebookTitle().Text(m_noteDraft.title);
        NotebookBody().Text(m_noteDraft.body);
        NoteError().Text(L"");
        NoteForm().Visibility(Visibility::Visible);
        NewPanelNote().IsEnabled(false);
        RenderDraftFiles();
        DispatcherQueue().TryEnqueue([weak=get_weak()] {
            if (auto self=weak.get(); self && !self->m_windowClosed && self->NoteForm().Visibility()==Visibility::Visible)
                self->NotebookTitle().Focus(FocusState::Programmatic);
        });
    }

    void MainWindow::RenderDraftFiles()
    {
        DraftFiles().Children().Clear();
        for (auto const& file : m_noteDraft.files)
        {
            Grid row;
            ColumnDefinition name; name.Width({ 1, GridUnitType::Star }); row.ColumnDefinitions().Append(name);
            ColumnDefinition action; action.Width(GridLengthHelper::Auto()); row.ColumnDefinitions().Append(action);
            TextBlock label;
            label.Text(file.filename().wstring());
            label.TextTrimming(TextTrimming::CharacterEllipsis);
            label.VerticalAlignment(VerticalAlignment::Center);
            ToolTipService::SetToolTip(label, box_value(file.wstring()));
            row.Children().Append(label);
            Button unlink;
            unlink.Content(box_value(L"×"));
            ToolTipService::SetToolTip(unlink, box_value(L"Unlink file"));
            unlink.Click([this, file](auto const&, auto const&) { std::erase(m_noteDraft.files, file); RenderDraftFiles(); });
            Grid::SetColumn(unlink, 1);
            row.Children().Append(unlink);
            DraftFiles().Children().Append(row);
        }
    }

    void MainWindow::RenderNoteCards()
    {
        NoteCards().Children().Clear();
        auto query = Fold(std::wstring(NotebookSearch().Text()));
        for (bool resolved : {false,true})
        for (auto const& note : m_store->PanelNotes())
        {
            if (note.resolved!=resolved) continue;
            if (!query.empty() && !Fold(note.title).contains(query) && !Fold(note.body).contains(query)) continue;
            Border card;
            card.CornerRadius({ 11 }); card.Padding({ 12 }); card.Background(UiBrush(L"CardBrush"));
            card.BorderBrush(UiBrush(note.id == m_noteFilter ? L"AccentBrush" : L"BorderBrush")); card.BorderThickness({ 1 });
            StackPanel stack; stack.Spacing(7);
            TextBlock title; title.Text(note.title); title.FontSize(15);
            title.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
            title.Foreground(UiBrush(L"InkBrush")); title.FontFamily(Microsoft::UI::Xaml::Media::FontFamily(m_store->Settings().Look().fonts[::HudesNative::AppFont].family));
            title.TextWrapping(TextWrapping::Wrap);
            Grid heading;
            ColumnDefinition markColumn; markColumn.Width(GridLengthHelper::Auto()); heading.ColumnDefinitions().Append(markColumn);
            ColumnDefinition titleColumn; titleColumn.Width({1,GridUnitType::Star}); heading.ColumnDefinitions().Append(titleColumn);
            if(note.resolved)
            {
                TextBlock tick; tick.Text(L"✓"); tick.FontSize(24); tick.FontWeight(Windows::UI::Text::FontWeights::Bold());
                tick.Foreground(UiBrush(L"ResolvedBrush")); tick.VerticalAlignment(VerticalAlignment::Center);
                tick.Margin({0,0,7,0}); heading.Children().Append(tick);
                Automation::AutomationProperties::SetName(tick,L"Resolved");
            }
            Grid::SetColumn(title,1); heading.Children().Append(title); stack.Children().Append(heading);
            if (!note.body.empty())
            {
                TextBlock body; body.Text(note.body); body.TextWrapping(TextWrapping::Wrap); body.MaxLines(5); body.Foreground(UiBrush(L"InkBrush")); body.FontFamily(title.FontFamily()); body.FontSize(m_store->Settings().Look().fonts[::HudesNative::AppFont].size);
                body.TextTrimming(TextTrimming::CharacterEllipsis); stack.Children().Append(body);
            }
            Button filter;
            auto active = note.id == m_noteFilter;
            filter.Foreground(UiBrush(L"InkBrush"));
            filter.Background(UiBrush(active ? L"SelectedBrush" : L"ControlBrush"));
            filter.Content(box_value((active ? L"Clear file filter" : L"Show files") + std::wstring(L" · ") + std::to_wstring(note.files.size())));
            filter.Click([this, id = note.id](auto const&, auto const&)
            {
                m_noteFilter = m_noteFilter == id ? L"" : id;
                RenderNoteCards(); RefreshFiles(); UpdateAddress();
            });
            stack.Children().Append(filter);
            MenuFlyout menu;
            MenuFlyoutItem edit; edit.Text(L"Edit note");
            edit.Click([this, id = note.id](auto const&, auto const&)
            {
                auto const& notes = m_store->PanelNotes();
                if (auto found = std::ranges::find(notes, id, &::HudesNative::PanelNote::id); found != notes.end()) EditPanelNote(*found);
            });
            menu.Items().Append(edit);
            MenuFlyoutItem resolve; resolve.Text(note.resolved?L"Mark unresolved":L"Mark resolved");
            resolve.Click([this,id=note.id](auto const&,auto const&)
            {
                try
                {
                    auto const& notes=m_store->PanelNotes();
                    auto found=std::ranges::find(notes,id,&::HudesNative::PanelNote::id);
                    if(found==notes.end()) return;
                    auto changed=*found; changed.resolved=!changed.resolved;
                    m_store->SavePanelNote(std::move(changed)); RenderNoteCards();
                }catch(...){ ShowMessage(L"Notes",::HudesNative::FriendlyError()); }
            });
            menu.Items().Append(resolve);
            MenuFlyoutItem remove; remove.Text(L"Delete note");
            remove.Click([this,id=note.id,title=note.title](auto const&,auto const&)
            {
                if(MessageBoxW(WindowHandle(),(L"Delete “"+title+L"”? Linked files will be kept.").c_str(),L"Delete note",MB_YESNO|MB_DEFBUTTON2|MB_ICONQUESTION)!=IDYES) return;
                try
                {
                    m_store->DeletePanelNote(id);
                    if(m_noteFilter==id){m_noteFilter.clear(); RefreshFiles(); UpdateAddress();}
                    RenderNoteCards();
                }catch(...){ShowMessage(L"Notes",::HudesNative::FriendlyError());}
            });
            menu.Items().Append(remove);
            menu.Opened([this](auto const&,auto const&){m_noteMenu=true;m_notesTimer.Stop();});
            menu.Closed([this](auto const&,auto const&){m_noteMenu=false;});
            card.ContextFlyout(menu);
            card.Child(stack); NoteCards().Children().Append(card);
        }
        if (!NoteCards().Children().Size())
        {
            TextBlock empty;
            empty.Text(query.empty() ? L"Add your first note with + New." : L"No matching notes.");
            empty.TextWrapping(TextWrapping::Wrap); empty.Foreground(UiBrush(L"MutedBrush")); NoteCards().Children().Append(empty);
        }
    }
}

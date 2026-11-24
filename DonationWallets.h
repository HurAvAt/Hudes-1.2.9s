#pragma once

#include <windows.h>
#include <cwchar>

namespace HudesNative::Donations
{
    inline constexpr wchar_t message[] = L"if you like the app please donate if you can to keep the development going :)";

    struct Wallet
    {
        wchar_t const* name;
        wchar_t const* icon;
        wchar_t const* address;
    };

    inline constexpr Wallet wallets[]{
        {L"Bitcoin", L"bitcoin", L"bc1qnmtqx3hxp4c5dtcurkg9fkppt2lnsrzq453y4j"},
        {L"Ethereum", L"ethereum", L"0x289E728cF99C36972f54D0B42c69C175Ab0dfAb3"},
        {L"Tron", L"tron", L"TMHDnj9GmQaGrZmFaeJtAkkuEZAPYTcwPX"},
        {L"Solana", L"solana", L"6edjdvpErjWVqWbYUR7iBwCbdNzQyQJDG27iQq8yepAE"}
    };

    inline bool CopyAddress(HWND window, Wallet const& wallet) noexcept
    {
        const size_t characters = std::wcslen(wallet.address) + 1;
        auto block = GlobalAlloc(GMEM_MOVEABLE, characters * sizeof(wchar_t));
        if (!block) return false;
        bool copied{};
        if (auto text = static_cast<wchar_t*>(GlobalLock(block)))
        {
            std::wmemcpy(text, wallet.address, characters);
            GlobalUnlock(block);
            if (OpenClipboard(window))
            {
                copied = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, block) != nullptr;
                CloseClipboard();
            }
        }
        // Windows owns the allocation once SetClipboardData succeeds.
        if (!copied) GlobalFree(block);
        return copied;
    }
}

# Hudes

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="Assets/hudes-logo-dark.svg">
  <img src="Assets/hudes-logo.svg" alt="Hudes" width="250">
</picture>

Hudes is a Windows file manager for people who keep track of work by folder. Choose a folder as your Desktop, then give its files notes, tags, a group, or a reminder. You can still browse the rest of your PC from the same window.

## Getting started

Hudes 1.2.9s Donate edition runs on Windows 11 x64.

- **Installer:** Open the MSI, then launch **Hudes Donate** from Start. It installs for your Windows account under `%LocalAppData%\Programs\Hudes Donate`.
- **Portable:** Extract the whole ZIP to a writable folder and open `Hudes.exe`. Keep the files next to the executable together.

The first sign-in password is `1111`. Choose the folder you want Hudes to manage, then change the password in Settings. Fresh workspaces open in light mode. The sun or moon in the header switches themes. Select the Hudes logo to lock the workspace.

## Donations

On first launch, choose **Donate** to view the wallets or **Maybe later** to continue. The coral **Donate** button at the left of the file filters opens the same four addresses at any time. Choose the network, then use **Copy** beside its address. Donations are optional.

## Everyday use

- **Files.** Right-click for common file actions. The **+** button creates a folder or a blank file. Drag across empty space to select several items. Ctrl+C, Ctrl+X, Ctrl+V, Delete and F2 work in the file view. Right-click **Show hidden files** to include Windows-hidden items and names beginning with a dot or tilde.
- **Groups and tags.** An item can have one group and any number of tags. Click a tag to filter the file view. Click it again or use **Clear filters** to return. All descendants follow the top folder’s group. A child with no edits follows the folder’s note and tags. Once you edit a child’s note or tags, it keeps them and picks up newly added parent tags. You can remove inherited tags from that child. Setting a child’s status separately also keeps it independent. The same rules apply when you move an item into an annotated folder.
- **Notes.** The left panel describes the selected item. The Notes panel on the right holds separate notes you can link to several files. Choose **+ New**, select the files, then choose **Link selected files**. **Show files** opens those links together. Right-click a note to edit, resolve, reopen, or delete it. Deleting a note keeps its files.
- **Archive.** Archiving changes where an item appears in Hudes while keeping its path on disk. Archived items appear under their group or **unGrouped**. To make a real copy of this layout, use **Workspace Export** in Settings. Choose CSV or XLSX for the accompanying metadata.
- **Shortcuts.** The twelve slots across the top can open files or apps and run CMD or Bash commands. Configure them in Settings. Hudes copies imported shortcut icons into its own folder.
- **Terminal.** Open the bottom rail for a shell in the current folder. **Terminal here** in the context menu starts another session at the selected location. Use **+** in the terminal to choose an installed shell for a new tab. Select a tab to return to its session. Settings lists supported shells already installed on the PC. The terminal display uses Microsoft Edge WebView2 Runtime.
- **Panels.** Drag the inner edge of File details or Notes, or the top edge of Terminal, to resize it. Pin a panel to reserve its space. File details starts pinned. A note draft stays open while you select and link files. Use **Save note** or **Cancel** to finish.
- **Appearance.** In Settings, expand **Light mode** or **Dark mode** to change its fonts, text colors, backgrounds, icons, borders, status colors, terminal colors, and resolved checkmark. **Save settings** applies both profiles.

## Database and backups

Hudes keeps its live database in `hudes.db.json` beside the executable. Use **Settings → Database** to back it up, restore it, or set a database password. The app lock and the database password serve different purposes. A database password encrypts the contents. Without one, the binary database uses compression and can be decoded.

Hudes accepts `.hudesdb`, older Salif `.salifdb` backups, and legacy JSON. A backup contains metadata and settings. The files themselves stay in their original folders. When moving a portable copy, include `ShortcutIcons` with the database so custom shortcut art travels with it. Back up the database before replacing an app folder.

## Build from source

Use Visual Studio 2026 with **Desktop development with C++**, the **C++ WinUI tools**, and the **Windows 11 SDK**. The first build restores NuGet packages. In PowerShell 7, from the repository root:

```powershell
./Tools/Build.ps1 -Test
```

The result is in `.build/Release`. You can also open `Hudes.slnx` and build **Release | x64**. To run the resource-lifetime checks with the debug C++ heap:

```powershell
./Tools/Build.ps1 -Configuration Debug -Test
```

To package the portable ZIP, MSI and source ZIP, install the WiX 4 CLI and supply a new output directory:

```powershell
dotnet tool install --global wix --version 4.0.6
./Tools/Package.ps1 -OutputRoot ../Hudes-release
```

The vectors in `Assets` are the originals for the H mark and lettering. Run `Tools/GenerateBrand.ps1` after editing them to rebuild the window artwork and multi-size Windows icon.

## License

Hudes is distributed under the GNU General Public License v3. The full text is in `LICENSE`. The terminal display includes xterm.js and addon-fit under their MIT licenses, included in `Assets/Terminal`.

Design by HurAvAt.

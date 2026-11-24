param(
    [Parameter(Mandatory)][string]$OutputRoot,
    [string]$BuildOutput,
    [string]$Wix = 'wix'
)
$ErrorActionPreference = 'Stop'
$project = Split-Path -Parent $PSScriptRoot
if (!$BuildOutput) { $BuildOutput = Join-Path $project '.build/Release' }
$build = (Resolve-Path -LiteralPath $BuildOutput).Path
$version = ([version](Get-Item -LiteralPath "$build/Hudes.exe").VersionInfo.FileVersion).ToString()
$manifest = [xml](Get-Content -LiteralPath "$project/app.manifest" -Raw)
if (([version]$manifest.assembly.assemblyIdentity.version).ToString() -ne $version) { throw 'Build version differs from the manifest.' }
if (!(Get-Content -LiteralPath "$build/self-test-results.txt" -Raw).StartsWith('PASS:')) { throw 'Run Build.ps1 -Test first.' }
if (!(Select-String -LiteralPath "$build/resource-test-results.txt" -Pattern '^PASS:' -Quiet)) { throw 'Run the resource tests before packaging.' }
$display = (Get-Item -LiteralPath "$build/Hudes.exe").VersionInfo.ProductVersion
if ($display -notmatch '^\d+\.\d+\.\d+[a-z]?$') { throw 'Invalid release label.' }
$tag = "Hudes-$display-donate"
$out = [IO.Path]::GetFullPath($OutputRoot)
if (Test-Path -LiteralPath $out) { throw "Choose a new output folder: $out" }
$stage = Join-Path $project ('.build/release-' + [guid]::NewGuid().ToString('N'))
$portable = Join-Path $stage "$tag-win-x64"
$source = Join-Path $stage $tag
$artifacts = Join-Path $stage 'artifacts'
New-Item -ItemType Directory -Path $portable,$source,$artifacts | Out-Null

Get-ChildItem -LiteralPath $build -Force | Where-Object {
    $_.Name -notmatch '^(hudes|salif)\.db\.json|^\.hudes-donate-welcome$|^ShortcutIcons$|^logo$|^(self|resource)-test-results\.txt$|\.salifdb($|\.)|\.hudesdb($|\.)|\.(pdb|exp|lib|ilk|wixpdb)$'
} | Copy-Item -Destination $portable -Recurse
Copy-Item -LiteralPath "$project/README.md","$project/GUIDE.txt" -Destination $portable -Force
Get-ChildItem -LiteralPath $project -File -Force | Where-Object {
    $_.Extension -in '.h','.cpp','.xaml','.idl','.vcxproj','.slnx','.manifest','.rc','.md','.txt' -or $_.Name -in 'LICENSE','.gitignore'
} | Copy-Item -Destination $source
Copy-Item -LiteralPath "$project/Assets","$project/Tools" -Destination $source -Recurse
Compress-Archive -LiteralPath $portable -DestinationPath "$artifacts/$tag-win-x64.zip"
Compress-Archive -LiteralPath $source -DestinationPath "$artifacts/$tag-source.zip"
& "$PSScriptRoot/MakeInstaller.ps1" -Payload $portable -Destination "$artifacts/$tag-win-x64.msi" -Wix $Wix

New-Item -ItemType Directory -Path (Split-Path -Parent $out) -Force | Out-Null
Move-Item -LiteralPath $artifacts -Destination $out
Get-ChildItem -LiteralPath $out | Select-Object Name,Length

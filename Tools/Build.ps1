param([string]$VisualStudioPath, [string]$PackageSource, [switch]$Test, [ValidateSet("Release","Debug")][string]$Configuration="Release")
$ErrorActionPreference='Stop'
$projectPath=Split-Path -Parent $PSScriptRoot
if (!$VisualStudioPath) {
    $vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) { $VisualStudioPath = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath }
}
if (!$VisualStudioPath) { throw 'Install Visual Studio 2026 C++/WinUI tools or pass -VisualStudioPath.' }
$msbuild=Join-Path $VisualStudioPath 'MSBuild/Current/Bin/MSBuild.exe'
if (!(Test-Path -LiteralPath $msbuild)) { throw "MSBuild is absent: $msbuild" }
$buildPath=Join-Path $projectPath '.build'
$releasePath=Join-Path $buildPath $Configuration
$objectPath=Join-Path $buildPath ('obj-'+$Configuration)
New-Item -ItemType Directory -Path $releasePath,$objectPath -Force | Out-Null
$restoreOptions=@()
if ($PackageSource) { $restoreOptions += "/p:RestoreSources=$((Resolve-Path -LiteralPath $PackageSource).Path)" }
& $msbuild (Join-Path $projectPath 'Hudes.vcxproj') /restore /m "/p:Configuration=$Configuration" /p:Platform=x64 "/p:RestorePackagesPath=$buildPath/nuget" "/p:OutDir=$releasePath\" "/p:IntDir=$objectPath\" @restoreOptions /verbosity:minimal "/flp:LogFile=$buildPath/build.log;Verbosity=normal"
if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE). See $buildPath/build.log" }
$crtRoots=Get-ChildItem -LiteralPath (Join-Path $VisualStudioPath 'VC/Redist/MSVC') -Directory -ErrorAction SilentlyContinue | Where-Object Name -match '^\d+\.' | Sort-Object { [version]$_.Name } -Descending
if ($crtRoots) {
    $crt=Get-ChildItem -LiteralPath (Join-Path $crtRoots[0].FullName 'x64') -Directory | Where-Object Name -like 'Microsoft.VC*.CRT' | Select-Object -First 1
    if ($crt) { Get-ChildItem -LiteralPath $crt.FullName -Filter '*.dll' | Copy-Item -Destination $releasePath -Force }
}
Copy-Item -LiteralPath (Join-Path $projectPath 'LICENSE'),(Join-Path $projectPath 'GUIDE.txt'),(Join-Path $projectPath 'CHANGELOG.md') -Destination $releasePath -Force
$licenseRoot=Join-Path $releasePath 'ThirdPartyLicenses'
Get-ChildItem -LiteralPath (Join-Path $buildPath 'nuget') -Directory | ForEach-Object {
    $packageName=$_.Name
    Get-ChildItem -LiteralPath $_.FullName -Directory | ForEach-Object {
        $destination=Join-Path $licenseRoot "$packageName/$($_.Name)"
        $notices=Get-ChildItem -LiteralPath $_.FullName -File | Where-Object Name -match '(?i)license|notice'
        if($notices){New-Item -ItemType Directory -Path $destination -Force | Out-Null; $notices | Copy-Item -Destination $destination -Force}
    }
}
if ($Test) {
    $run=Start-Process -FilePath (Join-Path $releasePath 'Hudes.exe') -ArgumentList '--self-test' -WindowStyle Hidden -Wait -PassThru
    Get-Content -LiteralPath (Join-Path $releasePath 'self-test-results.txt')
    if ($run.ExitCode -ne 0) { throw "Core tests failed ($($run.ExitCode))." }
    $run=Start-Process -FilePath (Join-Path $releasePath 'Hudes.exe') -ArgumentList '--resource-test' -WindowStyle Hidden -Wait -PassThru
    Get-Content -LiteralPath (Join-Path $releasePath 'resource-test-results.txt')
    if ($run.ExitCode -ne 0) { throw "Resource tests failed ($($run.ExitCode))." }
}
Write-Output "Release output: $releasePath"

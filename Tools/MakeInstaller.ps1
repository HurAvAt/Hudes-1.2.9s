param(
    [Parameter(Mandatory)][string]$Payload,
    [Parameter(Mandatory)][string]$Destination,
    [string]$Wix = 'wix'
)
$ErrorActionPreference = 'Stop'
$payloadPath = (Resolve-Path -LiteralPath $Payload).Path
$exe = Get-Item -LiteralPath (Join-Path $payloadPath 'Hudes.exe')
$version = ([version]$exe.VersionInfo.FileVersion).ToString()
$display = [Security.SecurityElement]::Escape($exe.VersionInfo.ProductVersion)
$work = Join-Path ([IO.Path]::GetTempPath()) ("hudes-msi-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work | Out-Null

function Escape-Xml([string]$s) { [Security.SecurityElement]::Escape($s) }
function Stable-Key([string]$s) {
    $bytes = [Text.Encoding]::UTF8.GetBytes("HurAvAt/HudesDonate/Windows/x64/" + $s.ToLowerInvariant())
    [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($bytes)).Substring(0,32)
}
function Directory-Id([string]$path) {
    if ($path -eq '.') { return 'INSTALLFOLDER' }
    return 'd' + (Stable-Key $path)
}
function Directory-Tree([string]$path) {
    foreach ($dir in (Get-ChildItem -LiteralPath $path -Directory | Sort-Object Name)) {
        $id = Directory-Id ([IO.Path]::GetRelativePath($payloadPath,$dir.FullName))
        '<Directory Id="{0}" Name="{1}">' -f $id,(Escape-Xml $dir.Name)
        Directory-Tree $dir.FullName
        '</Directory>'
    }
}

try {
    $directories = (Directory-Tree $payloadPath) -join [Environment]::NewLine
    $directoryIds = @('UserPrograms','INSTALLFOLDER') + @(Get-ChildItem -LiteralPath $payloadPath -Directory -Recurse | ForEach-Object {
        Directory-Id ([IO.Path]::GetRelativePath($payloadPath,$_.FullName))
    })
    $folderCleanup = ($directoryIds | ForEach-Object { '<RemoveFolder Id="r{0}" Directory="{0}" On="uninstall"/>' -f $_ }) -join [Environment]::NewLine
    $components = foreach ($file in (Get-ChildItem -LiteralPath $payloadPath -File -Recurse -Force | Sort-Object FullName)) {
        $relative = [IO.Path]::GetRelativePath($payloadPath,$file.FullName)
        $key = Stable-Key $relative
        $dir = Directory-Id ([IO.Path]::GetRelativePath($payloadPath,$file.DirectoryName))
        $remove = ''
        $shortcut = if ($relative -eq 'Hudes.exe') {
            $remove = $folderCleanup
            '<Shortcut Id="StartHudes" Directory="ProgramMenuFolder" Name="Hudes Donate" WorkingDirectory="INSTALLFOLDER" Icon="HudesIcon" Advertise="no"/>'
        } else { '' }
        $guid = [guid]::ParseExact($key,'N').ToString('B')
        @"
<Component Id="c$key" Directory="$dir" Guid="$guid">
 <File Id="f$key" Source="$(Escape-Xml $file.FullName)">$shortcut</File>
 <RegistryValue Root="HKCU" Key="Software\HurAvAt\HudesDonate\Installer" Name="$key" Type="integer" Value="1" KeyPath="yes"/>
 $remove
</Component>
"@
    }
    $source = @"
<Wix xmlns="http://wixtoolset.org/schemas/v4/wxs">
 <Package Name="Hudes $display – Donate" Manufacturer="HurAvAt" Version="$version" Scope="perUser" Language="1033"
          UpgradeCode="{BC94F106-AFA3-4DC4-9CD8-5EA180C98F31}" InstallerVersion="500">
  <MajorUpgrade AllowSameVersionUpgrades="yes" DowngradeErrorMessage="A newer version of Hudes is already installed." Schedule="afterInstallInitialize"/>
  <MediaTemplate EmbedCab="yes" CompressionLevel="high"/>
  <Icon Id="HudesIcon" SourceFile="$(Escape-Xml (Join-Path $payloadPath 'Assets/hudes.ico'))"/>
  <Property Id="ARPPRODUCTICON" Value="HudesIcon"/>
  <StandardDirectory Id="LocalAppDataFolder"><Directory Id="UserPrograms" Name="Programs"><Directory Id="INSTALLFOLDER" Name="Hudes Donate">$directories</Directory></Directory></StandardDirectory>
  <StandardDirectory Id="ProgramMenuFolder"/>
  <Feature Id="HudesApp" Title="Hudes" Level="1"><ComponentGroupRef Id="AppFiles"/></Feature>
 </Package>
 <Fragment><ComponentGroup Id="AppFiles">$($components -join [Environment]::NewLine)</ComponentGroup></Fragment>
</Wix>
"@
    $wxs = Join-Path $work 'Hudes.wxs'
    [IO.File]::WriteAllText($wxs,$source)
    & $Wix build $wxs -arch x64 -intermediateFolder $work -pdbtype none -o ([IO.Path]::GetFullPath($Destination))
    if ($LASTEXITCODE) { throw "WiX returned $LASTEXITCODE." }
} finally {
    $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
    if ([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($work)) -eq $tempRoot -and (Split-Path $work -Leaf) -match '^hudes-msi-[0-9a-f]{32}$') {
        Remove-Item -LiteralPath $work -Recurse -Force
    }
}

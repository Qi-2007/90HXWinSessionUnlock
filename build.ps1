[CmdletBinding()]
param([string]$MSBuild,[string]$SdkVersion,[switch]$Clean,[switch]$IncludeLegacyFilter)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'tools\BuildCommon.ps1')
$out=Join-Path $PSScriptRoot 'build\session-driver'
New-Item -ItemType Directory -Path $out -Force | Out-Null
foreach($name in @('CMP90HXUnlocker.exe','CMP90HXLowerFilter.sys','CMP90HXLowerFilter.inf','CMP90HXUnlockSession.sys','CMP90HXUnlockSession.inf','README.md')) {
    $path=Join-Path $out $name
    if(Test-Path -LiteralPath $path) {
        $stream=$null
        try { $stream=[IO.File]::Open($path,[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None) }
        finally { if($stream) { $stream.Dispose() } }
    }
}
$MSBuild=Resolve-BuildMSBuild $MSBuild -Driver
$target=if($Clean) {'/t:Rebuild'} else {'/t:Build'}
& $MSBuild (Join-Path $PSScriptRoot 'src\CMP90HXSessionUnlockProgram.csproj') $target /m /p:Configuration=Release /p:Platform=x64 /verbosity:minimal
if($LASTEXITCODE -ne 0) { throw 'Session unlock worker build failed.' }
$worker=Join-Path $PSScriptRoot 'build\CMP90HXUnlocker.exe'
& $worker help | Out-Host
if($LASTEXITCODE -ne 0) { throw 'Standalone worker help failed.' }
$hash=(Get-FileHash -LiteralPath $worker).Hash
$policy=Join-Path $PSScriptRoot 'build\caller-policy.h'
$content=Get-CallerPolicyContent $hash
if(!(Test-Path -LiteralPath $policy) -or [IO.File]::ReadAllText($policy) -ne $content) { [IO.File]::WriteAllText($policy,$content,[Text.Encoding]::ASCII) }
& (Join-Path $PSScriptRoot 'tools\build-filter-firmware.ps1')
$kit=(Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots').KitsRoot10
if(!$SdkVersion) {
    $versions=@(Get-ChildItem -LiteralPath (Join-Path $kit 'Include') -Directory | Where-Object {
        (Test-Path -LiteralPath (Join-Path $_.FullName 'km\ntddk.h')) -and (Test-Path -LiteralPath (Join-Path $kit "Lib\$($_.Name)\km\x64\ntoskrnl.lib"))
    } | Sort-Object { [version]$_.Name } -Descending)
    if(!$versions.Count) { throw 'Matching SDK/WDK not found.' }
    $SdkVersion=$versions[0].Name
}
$flavors=@('CMP90HXUnlockSession'); if($IncludeLegacyFilter) { $flavors+= 'CMP90HXLowerFilter' }
foreach($flavor in $flavors) {
    & $MSBuild (Join-Path $PSScriptRoot "driver\$flavor.vcxproj") $target /m /p:Configuration=Release /p:Platform=x64 "/p:WindowsTargetPlatformVersion=$SdkVersion" "/p:OutDir=$out\" /verbosity:minimal
    if($LASTEXITCODE -ne 0) { throw "$flavor build failed." }
    Assert-DriverCallerPolicy (Join-Path $out "$flavor.sys") $hash
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot "driver\$flavor.inf") -Destination $out -Force
    & (Join-Path $kit "Tools\$SdkVersion\x64\infverif.exe") /w (Join-Path $out "$flavor.inf")
    if($LASTEXITCODE -ne 0) { throw "$flavor INF validation failed." }
}
Copy-Item -LiteralPath $worker,(Join-Path $PSScriptRoot 'LICENSE') -Destination $out -Force
if(Test-Path -LiteralPath ($worker+'.config')) { Copy-Item -LiteralPath ($worker+'.config') -Destination $out -Force }
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'docs\session-unlock-test.md') -Destination (Join-Path $out 'README.md') -Force
# Catalog only selected packages; legacy INFs in the fixed output directory are not formal inputs.
$catalog=Join-Path $PSScriptRoot ('build\catalog-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $catalog | Out-Null
foreach($flavor in $flavors) {
    Copy-Item -LiteralPath (Join-Path $out "$flavor.sys"),(Join-Path $out "$flavor.inf") -Destination $catalog
}
& (Join-Path $kit "bin\$SdkVersion\x86\Inf2Cat.exe") "/driver:$catalog" /os:10_GE_X64 /uselocaltime
if($LASTEXITCODE -ne 0) { throw 'Catalog generation failed.' }
foreach($flavor in $flavors) { Copy-Item -LiteralPath (Join-Path $catalog "$flavor.cat") -Destination $out -Force }
$artifacts=@($flavors | ForEach-Object { Join-Path $out "$_.sys" }); $artifacts+=Join-Path $out 'CMP90HXUnlocker.exe'
Get-FileHash -LiteralPath $artifacts
Write-Host 'Unsigned Session package ready. Sign SYS, regenerate/sign CAT. No driver loaded. Historical Lower Filter builds require -IncludeLegacyFilter.'

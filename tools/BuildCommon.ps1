function Resolve-BuildMSBuild([string]$Explicit,[switch]$Driver) {
    if ($Explicit) {
        if (!(Test-Path -LiteralPath $Explicit -PathType Leaf)) { throw "MSBuild not found: $Explicit" }
        return [IO.Path]::GetFullPath($Explicit)
    }
    $vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (!(Test-Path -LiteralPath $vswhere)) { throw 'VS Installer/vswhere is missing.' }
    $instances=@((& $vswhere -all -products '*' -requires Microsoft.Component.MSBuild -format json) | ConvertFrom-Json)
    foreach ($instance in ($instances | Sort-Object { [version]$_.installationVersion } -Descending)) {
        $base=$instance.installationPath
        $exe=Join-Path $base 'MSBuild\Current\Bin\MSBuild.exe'
        if (!(Test-Path -LiteralPath $exe)) { continue }
        if ($Driver) {
            $vc=Join-Path $base 'MSBuild\Microsoft\VC'
            $toolsets=@(Get-ChildItem -LiteralPath $vc -Filter Toolset.props -Recurse -ErrorAction SilentlyContinue |
                Where-Object FullName -like '*\Platforms\x64\PlatformToolsets\WindowsKernelModeDriver10.0\Toolset.props')
            if (!$toolsets.Count) { continue }
        }
        return $exe
    }
    if ($Driver) { throw 'No MSBuild installation with x64 WindowsKernelModeDriver10.0 found. Install WDK VS integration or pass -MSBuild.' }
    throw 'MSBuild is unavailable.'
}
function Get-CallerPolicyContent([string]$Hash) {
    if ($Hash -cnotmatch '^[0-9A-F]{64}$') { throw 'Caller policy requires an uppercase SHA256 digest.' }
    $bytes=@(0..31 | ForEach-Object { '0x'+$Hash.Substring($_*2,2) }) -join ','
    return "/* Generated from the self-tested Gen2. Do not edit. */`r`n#define CMP_CALLER_SHA256_HEX `"$Hash`"`r`n#define CMP_CALLER_SHA256_BYTES {$bytes}`r`n"
}
function Assert-DriverCallerPolicy([string]$Path,[string]$WorkerHash) {
    # Read the policy from the actual signed binary, never trust a sidecar stamp.
    $content=[Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($Path))
    $matches=[regex]::Matches($content,'CMP90HX_CALLER_SHA256=([0-9A-F]{64})\x00')
    if ($matches.Count -ne 1 -or $matches[0].Groups[1].Value -cne $WorkerHash) {
        throw 'Driver caller policy does not match Gen2. Rebuild the driver with build-dma-driver.ps1, sign that new driver, and select it with -DriverPath.'
    }
}

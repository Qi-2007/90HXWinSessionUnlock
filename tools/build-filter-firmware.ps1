# Generate kernel C data from the same pinned firmware used by the source core.
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$items=@(
    @('firmware_plm_image.bin','firmware_plm_image','3df651acb174fd5f5561a1996965b192f50c5d763168446ed0d4ead19d895768'),
    @('firmware_plm_signature.bin','firmware_plm_signature','966512cef7f9ce3f4052b1bf33df7b509c97c405fdcf264df4ea2b1366c37372'),
    @('firmware_feature_image.bin','firmware_feature_image','3df651acb174fd5f5561a1996965b192f50c5d763168446ed0d4ead19d895768'),
    @('firmware_feature_signature.bin','firmware_feature_signature','966512cef7f9ce3f4052b1bf33df7b509c97c405fdcf264df4ea2b1366c37372'),
    @('firmware_booter_image.bin','firmware_booter_image','6803188fef6bd352f7ab47b2cf13e18175ac3f12243fa6afae7c4e2872f246f0'),
    @('firmware_booter_signatures.bin','firmware_booter_signatures','4db5a10949e475df7ecc011e34229895b296653cd401f6d88396ae00412cf0ea')
)
$text=New-Object Text.StringBuilder
[void]$text.AppendLine('/* Generated pinned GPU firmware data. No host executable code. */')
foreach($item in $items) {
    $path=Join-Path $root ('native\rebuilt-core\firmware\'+$item[0])
    if((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $item[2]) { throw "Firmware digest mismatch: $path" }
    $bytes=[IO.File]::ReadAllBytes($path)
    [void]$text.AppendLine(('const unsigned char '+$item[1]+'[] = {'))
    for($offset=0;$offset -lt $bytes.Length;$offset+=32) {
        $last=[Math]::Min($offset+31,$bytes.Length-1)
        [void]$text.AppendLine((($bytes[$offset..$last] | ForEach-Object { '0x'+$_.ToString('x2') }) -join ',')+',')
    }
    [void]$text.AppendLine('};')
}
$output=Join-Path $root 'build\filter-firmware.c'
$content=$text.ToString()
if(!(Test-Path -LiteralPath $output) -or [IO.File]::ReadAllText($output) -ne $content) {
    [IO.File]::WriteAllText($output,$content,(New-Object Text.UTF8Encoding($false)))
}

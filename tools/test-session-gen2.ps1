[CmdletBinding()]
param([string]$VcVars='H:\Program Files\VisualStudio\Community\VC\Auxiliary\Build\vcvars64.bat')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$out=Join-Path $root 'build\session-gen2-tests'
New-Item -ItemType Directory -Path $out -Force | Out-Null
$source=[IO.File]::ReadAllText((Join-Path $root 'driver\src\resume_unlock.h'))
$start=$source.IndexOf('static VOID CoreGen2Evidence(')
$end=$source.IndexOf('static BOOLEAN CoreCanReleaseForPnp(', $start)
if($start -lt 0 -or $end -le $start) { throw 'Cannot locate driver Gen2 functions.' }
$configStart=$source.IndexOf('static VOID CoreSbrConfigEvidence(')
$configEnd=$source.IndexOf('static int CoreSbr(', $configStart)
if($configStart -lt 0 -or $configEnd -le $configStart) { throw 'Cannot locate SBR config wait functions.' }
$underTest=$source.Substring($configStart,$configEnd-$configStart)+$source.Substring($start,$end-$start)
[IO.File]::WriteAllText((Join-Path $out 'gen2-under-test.h'),$underTest,[Text.Encoding]::ASCII)
if(!(Test-Path -LiteralPath $VcVars)) { throw "Missing VS environment script: $VcVars" }
$test=Join-Path $PSScriptRoot 'SessionGen2Tests.c'
# One cmd environment owns both VS setup and compilation. No hardware access.
$batch="@echo off`r`ncall `"$VcVars`" >nul`r`nif errorlevel 1 exit /b 1`r`ncl /nologo /W4 /WX /TC `"$test`" /I`"$out`" /Fo`"$out\SessionGen2Tests.obj`" /Fe`"$out\SessionGen2Tests.exe`"`r`nexit /b %errorlevel%`r`n"
$build=Join-Path $out 'compile.cmd'
[IO.File]::WriteAllText($build,$batch,[Text.Encoding]::Default)
& $env:ComSpec /d /c "`"$build`""
if($LASTEXITCODE -ne 0) { throw 'Gen2 host test compilation failed.' }
& (Join-Path $out 'SessionGen2Tests.exe')
if($LASTEXITCODE -ne 0) { throw 'Gen2 host regression tests failed.' }

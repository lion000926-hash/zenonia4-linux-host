$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$zig = $env:ZIG_EXE
if (-not $zig) {
    $candidates = @(
        'E:/Codex/2026-10-17/new-chat/work/toolchains/zig/zig-windows-x86_64-0.13.0/zig.exe',
        "$env:USERPROFILE\zig\zig.exe"
    )
    foreach ($c in $candidates) { if (Test-Path $c) { $zig = $c; break } }
}
if (-not $zig -or -not (Test-Path $zig)) {
    throw 'Set $env:ZIG_EXE to zig 0.13.0 (or install and update build.ps1).'
}

$src = Join-Path $root 'src'
$out = Join-Path $root 'zenonia4'
$files = @(
    "$src\main.c",
    "$src\runtime_symbols.c",
    "$src\loader.c",
    "$src\jni_bridge.c",
    "$src\audio.c",
    "$src\ui_layout.c",
    "$src\input_route.c",
    "$src\combat_input.c",
    "$src\profile.c",
    "$src\scene_view.c",
    "$src\popup_layout.c",
    "$src\dialogue_view.c",
    "$src\popup_view.c",
    "$src\cinematic_view.c",
    "$src\script_text.c",
    "$src\compress_blit.c",
    "$root\vendor\font.c"
)

& $zig cc -target aarch64-linux-gnu.2.31 -O2 -g -fno-omit-frame-pointer `
    -DGAME_W=800 -DGAME_H=600 `
    "-I$root\include" "-I$root\vendor" "-I$src" `
    @files -o $out -lc -lm -ldl -lpthread
if ($LASTEXITCODE -ne 0) { throw 'compile failed' }
Write-Host "OK $out"
Get-FileHash $out -Algorithm SHA256 | Format-List

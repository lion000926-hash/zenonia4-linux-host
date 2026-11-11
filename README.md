# Zenonia4 Linux Host

Community ARM64 Linux host / loader for a **locally owned** copy of *ZENONIA 4*.

This repository contains **only host source code** (ELF loader, JNI bridge, SDL2 presentation, input routing, CPU blit optimizations). It does **not** include the game binary, assets, audio, fonts, saves, or translation data packs.

## Rights / disclaimer

- *ZENONIA 4* and all related trademarks, artwork, scripts, and data belong to the original rights holders.
- This project is an unofficial community technical effort. It is **not** affiliated with, endorsed by, or authorized by the original company.
- **Do not** use this repository to redistribute proprietary game files.
- You must supply `libgameDSO.so`, `assets/`, `res/`, and a font from a copy you are legally allowed to use.
- Fan UI string overlays that may appear in source are unofficial and for personal research only.

## What you get

| Path | Purpose |
| --- | --- |
| `src/` | Host: loader, JNI bridge, UI/input hooks, CPU compress blit opts |
| `include/` | SDL2 / JNI / GLES headers used at build time |
| `vendor/` | Font raster helper adapted from the Vita community port (MIT) + `stb_truetype` |
| `build.ps1` | Cross-compile with Zig 0.13 → `aarch64-linux-gnu` |
| `launch.sh` | Example device launcher |

## Build

Requirements:

- Windows (or adjust the script) with [Zig 0.13.0](https://ziglang.org/download/)
- Set `ZIG_EXE` if Zig is not at the default path in `build.ps1`

```powershell
.\build.ps1
```

Output binary: `zenonia4` (aarch64 Linux).

## Runtime layout (not in this repo)

On device, next to the binary:

```
zenonia4                 # host binary from this project
libgameDSO.so            # from your own game package
font.ttf
assets/
res/
saves/
logs/
```

Useful flags:

- `--probe` — load / relocate / JNI smoke check
- `--check-compress` — pixel check for COPY/ADD compress blit fast path (no hooks)

## Credits

- Adapted font bridge ideas: [MetalSyntax/Zenonia4-psvita-port](https://github.com/MetalSyntax/Zenonia4-psvita-port) (MIT, see `vendor/Zenonia4-psvita-port.LICENSE`)
- [stb](https://github.com/nothings/stb) (`stb_truetype.h`)
- SDL2 / OpenJDK JNI headers retain upstream notices

Host code in `src/` is MIT-licensed; see `LICENSE`.

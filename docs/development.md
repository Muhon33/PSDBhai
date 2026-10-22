# PSDGlance developer guide

## How it works

PSDGlance is a Windows Shell thumbnail handler (`IThumbnailProvider` + `IInitializeWithStream`). Windows loads `PSDGlance.dll` in an isolated process (`dllhost.exe`) only when Explorer needs a thumbnail, so a damaged file can never crash Explorer.

For each request the DLL reads the file header (a few KB) and picks the cheapest source that is sharp enough:

1. **Embedded JPEG preview** (image resource 1036/1033), when the requested size is no bigger than it, typically 160 px or less.
2. **Composite image**, decoded at reduced size by seeking straight to the rows it needs. A 256 px thumbnail of a 750 MB PSD reads about 10 MB and takes about 12 ms.
3. **Upscaled JPEG preview**, if the file was saved without "Maximize Compatibility".
4. Otherwise it reports failure, and Explorer shows the normal icon.

Supported: every color mode (RGB, Grayscale, CMYK, Indexed, Lab, Bitmap, Duotone, Multichannel), 8/16/32-bit, RAW/RLE/ZIP/ZIP-with-prediction composites, and transparency.

## Build

Requirements:

- [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) (tested with release 20261020, `ucrt-x86_64`). It builds both x64 and ARM64.
- [Inno Setup 6](https://jrsoftware.org/isinfo.php), only for the installer.

```powershell
.\build.ps1 -Toolchain C:\path\to\llvm-mingw            # bin\x64 and bin\arm64
.\build.ps1 -Toolchain C:\path\to\llvm-mingw -Tests     # plus the test suite
.\build.ps1 -Toolchain C:\path\to\llvm-mingw -Installer # plus dist\PSDGlance-Setup-<version>.exe
```

Alternatively, set `LLVM_MINGW` instead of passing `-Toolchain`. The version comes from the `VERSION` file; the build writes it into every binary and the installer. Everything links statically, so the binaries depend only on DLLs that ship with Windows 10/11.

The code is plain Win32 C++17. It should build with MSVC, but no Visual Studio project is included and that path is untested.

## Tests

`.\build.ps1 -Tests` builds `bin\tests\psdglance_tests.exe`, which:

- generates 120 synthetic PSD/PSB files covering every color mode x bit depth x compression, PSD and PSB, and checks decoded pixels at full and reduced size;
- checks transparency rules, including that a saved selection channel is not treated as transparency, and layer counts stored in `Lr16`/`Lr32` blocks;
- checks thumbnail source selection, the BGR (1033) preview, and "no composite" fallbacks;
- tests the reader cache against random reads, the inflater against a real PNG's dynamic-Huffman stream, and PackBits round trips;
- runs about 4,000 truncated files and 20,000 randomly corrupted files (`-FuzzIterations N` to change the count).

`.\build.ps1 -Tests -Sanitize` runs the same suite under AddressSanitizer and UBSan. It has passed with 200,000 corrupted files.

## Developer CLI

`bin\x64\psdglance-cli.exe` uses exactly the same code as the DLL:

```text
psdglance-cli info   <file.psd>                                   header, preview, layers, compression
psdglance-cli render <file.psd> <out.png> [--size N] [--source auto|jpeg|composite] [--fast]
psdglance-cli dll    <PSDGlance.dll> <file.psd> <out.png> [--size N]   call the DLL via COM, no registration
psdglance-cli shell  <file.psd> <out.png> [--size N]              ask Explorer's real thumbnail pipeline
psdglance-cli bench  <file-or-folder> [--size N]                  time and bytes read per file
```

## Project layout

```text
src/core/       portable PSD/PSB parser and decoder (no Windows code)
src/win/        WIC JPEG decoding, source selection, IStream/file sources, registry
src/shellext/   PSDGlance.dll: COM class factory + IThumbnailProvider
src/settings/   PSDGlance.exe (also does /register and /unregister for the installer)
src/cli/        psdglance-cli.exe developer tool
tests/          synthetic-file test suite and fuzzer
installer/      Inno Setup script
res/            app icon (tools\make-icon.ps1) and README artwork
.signpath/      SignPath artifact configurations
```

## Registry

Per user, the handler writes under `HKCU\Software\Classes`; for all users, under `HKLM\Software\Classes`:

```text
CLSID\{6CDE7511-0277-42B6-8728-CF41F2F2C147}\InprocServer32   = <install dir>\PSDGlance.dll (Apartment)
.psd\ShellEx\{E357FCCD-A995-4576-B01F-234630154E96}            = {6CDE7511-0277-42B6-8728-CF41F2F2C147}
.psb\ShellEx\{E357FCCD-A995-4576-B01F-234630154E96}            = {6CDE7511-0277-42B6-8728-CF41F2F2C147}
```

The quality setting is `HKCU\Software\PSDGlance\Quality` (0 = Sharp, 1 = Fast). If a file type already had another thumbnail handler, it is saved under `Software\PSDGlance` and restored when PSDGlance is turned off or uninstalled.

## Releasing

Pushing to `main` builds, tests and packages the installer on GitHub Actions (download it from the run's **Artifacts**). To publish a release:

1. Change the version in the `VERSION` file, for example to `1.0.2`, and commit.
2. `git tag v1.0.2` and `git push origin v1.0.2`.
3. Once SignPath is set up, approve the two signing requests (program files, then installer) in SignPath.
4. The workflow publishes the GitHub Release with `PSDGlance-Setup-<version>.exe` and a fixed-name copy, `PSDGlance-Setup.exe`, so `https://github.com/Muhon33/PSDGlance/releases/latest/download/PSDGlance-Setup.exe` always downloads the newest version.

Signing setup: [signing.md](signing.md).

## Known limits

- CMYK and Lab colors use simple conversions, not the file's ICC profile, so CMYK can look slightly different from Photoshop.
- Files saved with both "Maximize Compatibility" and "Image Previews" off show the normal icon; layers are not rendered.
- Not built yet: Preview pane (Alt+P) and Details-view properties such as dimensions.

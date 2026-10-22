# PSDBhai

Shows real thumbnails for Photoshop **.psd** and **.psb** files in Windows File Explorer and in every app's Open/Save dialog, the way macOS Finder does.

- Tiny: a ~220 KB DLL plus a ~40 KB settings app; 2.2 MB installer.
- Nothing to install alongside it: no .NET, no Visual C++ runtime, no Photoshop.
- Nothing running in the background: Windows loads the DLL only when Explorer needs a thumbnail, in an isolated process (`dllhost.exe`), so a damaged file can never crash Explorer.
- Windows 10 and 11, on Intel/AMD (x64) and ARM64 PCs.
- No network access, no telemetry.

## Install (3 clicks, no admin password)

1. Double-click **PSDBhai-Setup-1.0.0.exe**.
2. If Windows shows "Windows protected your PC", click **More info**, then **Run anyway**. This appears because the installer isn't code-signed yet; it is not a virus warning.
3. Accept the license, then click **Install**, then **Finish**.

Open any folder with PSD files and choose View > Medium, Large or Extra large icons. If Explorer still shows the old icon for files it has seen before, open **PSDBhai** from the Start menu and click **Refresh thumbnails**.

To remove it: Settings > Apps > Installed apps > PSDBhai > Uninstall. Any thumbnail handler it replaced is restored.

For IT admins: `PSDBhai-Setup-1.0.0.exe /ALLUSERS` installs for everyone (needs admin); add `/VERYSILENT` for unattended installs.

## Settings app

| Option | What it does |
| --- | --- |
| Show thumbnails for .psd / .psb | Turns the handler on or off per file type |
| Sharp (default) | Small icons use Photoshop's built-in preview; large icons decode the full composite image |
| Fast | Always uses Photoshop's small built-in preview (fastest, softer at large sizes) |
| Refresh thumbnails | Closes and reopens File Explorer and clears the Windows thumbnail cache |

## How it works

For each thumbnail Windows asks for, the DLL reads the file header (a few KB) and picks the cheapest source that is sharp enough:

1. **Embedded JPEG preview** (resource 1036/1033), when the requested size is no bigger than it, typically 160 px or less.
2. **Composite image**, decoded at reduced size by seeking straight to the rows it needs. A 256 px thumbnail of a 750 MB PSD reads about 10 MB and takes about 12 ms.
3. **Upscaled JPEG preview**, if the file was saved without "Maximize Compatibility".
4. Otherwise it reports failure, and Explorer shows the normal icon.

It supports every color mode (RGB, Grayscale, CMYK, Indexed, Lab, Bitmap, Duotone, Multichannel), 8/16/32-bit depths, RAW/RLE/ZIP/ZIP-with-prediction composites, and transparency. The full design is in the developer handoff document.

## Build

Requirements:

- [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) (tested with release 20261020, `ucrt-x86_64`). It builds both x64 and ARM64.
- [Inno Setup 6](https://jrsoftware.org/isinfo.php), only for the installer.

```powershell
.\build.ps1 -Toolchain C:\path\to\llvm-mingw            # bin\x64 and bin\arm64
.\build.ps1 -Toolchain C:\path\to\llvm-mingw -Tests     # plus the test suite
.\build.ps1 -Toolchain C:\path\to\llvm-mingw -Installer # plus dist\PSDBhai-Setup-1.0.0.exe
```

Alternatively, set `LLVM_MINGW` instead of passing `-Toolchain`. Everything links statically (`-static`), so the binaries depend only on DLLs that ship with Windows 10/11.

The code is plain Win32 C++17 with no compiler-specific extensions beyond `__uuidof`. It should build with MSVC, but no Visual Studio project is included and that path is untested.

## Tests

`.\build.ps1 -Tests` builds `bin\tests\psdbhai_tests.exe`, which:

- generates 120 synthetic PSD/PSB files covering every color mode x bit depth x compression, PSD and PSB, and checks decoded pixels at full and reduced size;
- checks transparency rules, including that a saved selection channel is not treated as transparency, and layer counts stored in `Lr16`/`Lr32` blocks;
- checks thumbnail source selection, the BGR (1033) preview, and "no composite" fallbacks;
- tests the reader cache against random reads, the inflater against a real PNG's dynamic-Huffman stream, and PackBits round trips;
- runs about 4,000 truncated files and 20,000 randomly corrupted files (`-FuzzIterations N` to change the count).

`.\build.ps1 -Tests -Sanitize` runs the same suite under AddressSanitizer and UBSan to catch out-of-bounds reads that don't crash. It has passed with 200,000 corrupted files.

## Developer CLI

`bin\x64\psdbhai-cli.exe` uses exactly the same code as the DLL:

```text
psdbhai-cli info   <file.psd>                                   header, preview, layers, compression
psdbhai-cli render <file.psd> <out.png> [--size N] [--source auto|jpeg|composite] [--fast]
psdbhai-cli dll    <PSDBhai.dll> <file.psd> <out.png> [--size N]   call the DLL via COM, no registration
psdbhai-cli shell  <file.psd> <out.png> [--size N]              ask Explorer's real thumbnail pipeline
psdbhai-cli bench  <file-or-folder> [--size N]                  time and bytes read per file
```

## Project layout

```text
src/core/       portable PSD/PSB parser and decoder (no Windows code)
src/win/        WIC JPEG decoding, source selection, IStream/file sources, registry
src/shellext/   PSDBhai.dll: COM class factory + IThumbnailProvider
src/settings/   PSDBhai.exe (also does /register and /unregister for the installer)
src/cli/        psdbhai-cli.exe developer tool
tests/          synthetic-file test suite and fuzzer
installer/      Inno Setup script
res/            app icon (regenerate with tools\make-icon.ps1)
```

## Registry

Per user, the handler writes under `HKCU\Software\Classes`; for all users, under `HKLM\Software\Classes`:

```text
CLSID\{6CDE7511-0277-42B6-8728-CF41F2F2C147}\InprocServer32   = <install dir>\PSDBhai.dll (Apartment)
.psd\ShellEx\{E357FCCD-A995-4576-B01F-234630154E96}            = {6CDE7511-0277-42B6-8728-CF41F2F2C147}
.psb\ShellEx\{E357FCCD-A995-4576-B01F-234630154E96}            = {6CDE7511-0277-42B6-8728-CF41F2F2C147}
```

The quality setting is `HKCU\Software\PSDBhai\Quality` (0 = Sharp, 1 = Fast). If a file type already had another thumbnail handler, it is saved under `Software\PSDBhai` and restored when PSDBhai is turned off or uninstalled.

## Releasing

Pushing to `main` builds, tests and packages the installer on GitHub Actions (download it from the run's **Artifacts**). To publish a release:

1. Bump the version in `src/win/config.h` (`kVersion`), `src/shellext/dll.rc`, `src/settings/settings.rc` and `installer/psdbhai.iss`.
2. Commit, then `git tag v1.0.1` and `git push --tags`. The workflow attaches the installer to a GitHub Release.

To sign locally once you have a code-signing certificate:

```powershell
.\build.ps1 -Installer -SignCommand 'signtool sign /fd sha256 /tr <timestamp-url> /td sha256 /a {file}'
```

This signs both DLLs, the settings apps, the installer and the uninstaller.

## License

[MIT](LICENSE). The binaries also contain runtime code from LLVM and MinGW-w64; their notices are in [THIRD-PARTY-NOTICES.txt](THIRD-PARTY-NOTICES.txt), which the installer copies next to the app.

Photoshop is a registered trademark of Adobe Inc. PSDBhai is an independent project, not affiliated with or endorsed by Adobe.

## Known limits

- **Not code-signed yet.** Until the installer is signed, Windows SmartScreen shows "Windows protected your PC" when it's downloaded, and Windows 11 PCs with Smart App Control turned on may block it. Microsoft Defender finds no threats in it.
- CMYK and Lab colors use simple conversions, not the file's ICC profile, so CMYK can look slightly different from Photoshop.
- Files saved with both "Maximize Compatibility" and "Image Previews" off show the normal icon; layers are not rendered.
- v1.1 (not built yet): Preview pane (Alt+P) and Details-view properties such as dimensions.

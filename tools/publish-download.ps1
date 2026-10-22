# Publishes the built installer as a release of the public download repo
# (Muhon33/Download-PSDGlance), with a fixed-name copy so
# https://github.com/Muhon33/Download-PSDGlance/releases/latest/download/PSDGlance-Setup.exe
# always downloads the newest version.
#
#   .\build.ps1 -Installer              # build dist\PSDGlance-Setup-<VERSION>.exe first
#   .\tools\publish-download.ps1        # needs GitHub CLI, signed in once with: gh auth login
param([string]$Repo = 'Muhon33/Download-PSDGlance')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
$version = (Get-Content -Raw (Join-Path $root 'VERSION')).Trim()
$setup = Join-Path $root "dist\PSDGlance-Setup-$version.exe"
if (-not (Test-Path $setup)) { throw "dist\PSDGlance-Setup-$version.exe not found. Run .\build.ps1 -Installer first." }

$gh = (Get-Command gh -ErrorAction SilentlyContinue).Source
if (-not $gh) { $gh = Join-Path $env:ProgramFiles 'GitHub CLI\gh.exe' }
if (-not (Test-Path $gh)) { throw "GitHub CLI not found. Install it with: winget install GitHub.cli" }

$stable = Join-Path ([IO.Path]::GetTempPath()) 'PSDGlance-Setup.exe'
Copy-Item $setup $stable -Force

$notes = @"
Shows thumbnails for Photoshop PSD and PSB files in Windows File Explorer.

**Install:** download **PSDGlance-Setup.exe** below on your Windows PC, double-click it, accept the license, click **Install**. No admin password needed. Windows 10 and 11, Intel/AMD and ARM PCs.

**Seeing a warning?** PSDGlance isn't code-signed yet. If your browser says the file isn't commonly downloaded, choose **Keep**. If Windows shows "Windows protected your PC", click **More info**, then **Run anyway**.

**Uninstall:** Settings > Apps > Installed apps > PSDGlance > Uninstall.
"@

& $gh release create "v$version" $setup $stable --repo $Repo --title "PSDGlance $version" --notes $notes
if ($LASTEXITCODE -ne 0) { throw "gh release create failed" }
Write-Host "Published v$version to https://github.com/$Repo/releases/tag/v$version" -ForegroundColor Green

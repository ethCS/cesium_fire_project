param(
    [string]$Repo = "ethCS/cesium_fire_project",
    [string]$Tag = "project-assets-v1",
    [switch]$SkipContent,
    [switch]$SkipFireData
)

$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $PSScriptRoot
$downloadDir = Join-Path $projectRoot ".asset-downloads"

if (-not (Get-Command gh -ErrorAction SilentlyContinue)) {
    throw "GitHub CLI (gh) is required. Install from https://cli.github.com/"
}

New-Item -ItemType Directory -Force -Path $downloadDir | Out-Null

Push-Location $downloadDir
try {
    if (-not $SkipContent) {
        Write-Host "Downloading Content archive parts from release $Tag..."
        gh release download $Tag --repo $Repo --pattern "Content.zip.part*" --clobber

        $parts = Get-ChildItem "Content.zip.part*" | Sort-Object Name
        if ($parts.Count -eq 0) {
            throw "No Content.zip parts found in release $Tag."
        }

        $contentZip = Join-Path $projectRoot "Content.zip"
        if (Test-Path $contentZip) { Remove-Item $contentZip -Force }

        Write-Host "Rebuilding Content.zip from parts..."
        $out = [System.IO.File]::Create($contentZip)
        try {
            foreach ($part in $parts) {
                $in = [System.IO.File]::OpenRead($part.FullName)
                try {
                    $in.CopyTo($out)
                }
                finally {
                    $in.Dispose()
                }
            }
        }
        finally {
            $out.Dispose()
        }

        Write-Host "Extracting Content.zip..."
        Expand-Archive -Path $contentZip -DestinationPath $projectRoot -Force
    }

    if (-not $SkipFireData) {
        Write-Host "Downloading FireData.zip..."
        gh release download $Tag --repo $Repo --pattern "FireData.zip" --clobber

        $fireDataZip = Join-Path $downloadDir "FireData.zip"
        if (-not (Test-Path $fireDataZip)) {
            throw "FireData.zip not found in release $Tag."
        }

        Write-Host "Extracting FireData.zip..."
        Expand-Archive -Path $fireDataZip -DestinationPath $projectRoot -Force
    }
}
finally {
    Pop-Location
}

Write-Host "Done."
Write-Host "Installed assets into: $projectRoot"

[CmdletBinding()]
param(
    [string]$IdentityFile
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$RemoteUser = "yingying"
$RemoteHost = "mydoor.eastasia.cloudapp.azure.com"
$RemoteDirectory = "/home/yingying/onedrivecpp"
$ProjectRoot = Split-Path -Parent $PSScriptRoot
$ArchiveName = "onedrivecpp-$([guid]::NewGuid().ToString('N')).tar.gz"
$LocalArchive = Join-Path ([System.IO.Path]::GetTempPath()) $ArchiveName
$RemoteArchive = "/tmp/$ArchiveName"

foreach ($command in @("tar", "scp", "ssh")) {
    if (-not (Get-Command $command -ErrorAction SilentlyContinue)) {
        throw "Required command '$command' was not found in PATH."
    }
}

$SshOptions = @()
if ($IdentityFile) {
    $ResolvedIdentityFile = (Resolve-Path $IdentityFile).Path
    $SshOptions += @("-i", $ResolvedIdentityFile)
}

try {
    Write-Host "Creating source archive..."
    & tar @(
        "-czf", $LocalArchive,
        "--exclude=./build",
        "--exclude=./.git",
        "--exclude=./.cache",
        "--exclude=./compile_commands.json",
        "-C", $ProjectRoot,
        "."
    )
    if ($LASTEXITCODE -ne 0) {
        throw "tar failed with exit code $LASTEXITCODE."
    }

    $Destination = "${RemoteUser}@${RemoteHost}:$RemoteArchive"
    Write-Host "Uploading archive to $RemoteHost..."
    & scp @SshOptions $LocalArchive $Destination
    if ($LASTEXITCODE -ne 0) {
        throw "scp failed with exit code $LASTEXITCODE."
    }

    $RemoteCommand = "mkdir -p '$RemoteDirectory' && tar -xzf '$RemoteArchive' -C '$RemoteDirectory' && rm -f '$RemoteArchive'"
    Write-Host "Extracting files into $RemoteDirectory..."
    & ssh @SshOptions "${RemoteUser}@${RemoteHost}" $RemoteCommand
    if ($LASTEXITCODE -ne 0) {
        throw "ssh failed with exit code $LASTEXITCODE. The uploaded archive remains at $RemoteArchive."
    }

    Write-Host "Deployment completed: ${RemoteUser}@${RemoteHost}:$RemoteDirectory"
} finally {
    if (Test-Path $LocalArchive) {
        Remove-Item -LiteralPath $LocalArchive -Force
    }
}

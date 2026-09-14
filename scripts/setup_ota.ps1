[CmdletBinding(SupportsShouldProcess)]
param(
    [string]$ProjectRoot = (Split-Path -Parent $PSScriptRoot)
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$envPath = Join-Path $ProjectRoot '.env'
$credentialPath = Join-Path $ProjectRoot '.env.ota'
if (-not (Test-Path -LiteralPath $envPath)) {
    throw 'Create the private .env build configuration before setting up OTA.'
}

$envContent = [System.IO.File]::ReadAllText($envPath)
$hashSettings = [regex]::Matches($envContent, '(?m)^\s*(?:export\s+)?OTA_PASSWORD_HASH\s*=([^\r\n]*)')
$credentialExists = Test-Path -LiteralPath $credentialPath
if ($hashSettings.Count -gt 0 -and -not $credentialExists) {
    throw 'OTA_PASSWORD_HASH already exists. Keep the existing OTA password; do not replace it automatically.'
}

if (-not $PSCmdlet.ShouldProcess($ProjectRoot, 'Configure authenticated OTA using a private, reusable credential')) {
    return
}

if ($credentialExists) {
    $credentials = ConvertFrom-StringData -StringData ([System.IO.File]::ReadAllText($credentialPath))
    if (-not $credentials.ContainsKey('OTA_PASSWORD') -or $credentials.OTA_PASSWORD.Length -lt 32) {
        throw 'The existing .env.ota credential is invalid. It has not been overwritten.'
    }
    $password = $credentials.OTA_PASSWORD
} else {
    $passwordBytes = New-Object byte[] 32
    $generator = [System.Security.Cryptography.RandomNumberGenerator]::Create()
    try {
        $generator.GetBytes($passwordBytes)
    } finally {
        $generator.Dispose()
    }
    $password = [Convert]::ToBase64String($passwordBytes)
}

$digest = [System.Security.Cryptography.MD5]::Create()
try {
    $hashBytes = $digest.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($password))
    $passwordHash = -join ($hashBytes | ForEach-Object { $_.ToString('x2') })
} finally {
    $digest.Dispose()
}

if ($hashSettings.Count -gt 0) {
    $existingHash = $hashSettings[$hashSettings.Count - 1].Groups[1].Value.Trim().Trim('"').Trim("'")
    if ($existingHash -cne $passwordHash) {
        throw 'The existing OTA credential and build hash differ. Neither file has been changed.'
    }
    Write-Output 'OTA credential and build hash already match; no changes needed.'
    return
}

if (-not $credentialExists) {
    $stream = [System.IO.File]::Open($credentialPath, [System.IO.FileMode]::CreateNew, [System.IO.FileAccess]::Write)
    try {
        $content = [System.Text.Encoding]::UTF8.GetBytes("OTA_PASSWORD=$password`n")
        $stream.Write($content, 0, $content.Length)
    } finally {
        $stream.Dispose()
    }
}

[System.IO.File]::AppendAllText($envPath, "`nOTA_PASSWORD_HASH=`"$passwordHash`"`n")
Write-Output 'OTA configured. Password stored in .env.ota; only its hash is included in .env. Values not displayed.'
# publish.ps1 -- push this repository to GitHub with a short-lived token.
#
# Usage:
#   .\publish.ps1 -Token <PAT> [-Repo HanaGuitar] [-Private]
#
# The token is used only for this run: it goes into an ephemeral HTTP header for
# the push and is never written to .git/config.

param(
    [Parameter(Mandatory = $true)][string]$Token,
    [string]$Repo = "HanaGuitar",
    [switch]$Private
)

$ErrorActionPreference = "Stop"
$git = "D:\harness2號工作區\.tools\git\cmd\git.exe"
if (-not (Test-Path $git)) { $git = "git" }

$headers = @{
    "Authorization" = "Bearer $Token"
    "User-Agent"    = "HanaGuitar-publish"
    "Accept"        = "application/vnd.github+json"
}

Write-Host "== who am I =="
$me = Invoke-RestMethod -Uri "https://api.github.com/user" -Headers $headers
Write-Host "  $($me.login)"

Write-Host "== repository =="
$body = @{ name = $Repo; private = [bool]$Private; description = "HanaGuitar for indie - physical offset guitar (VST3)" } | ConvertTo-Json
try {
    $r = Invoke-RestMethod -Uri "https://api.github.com/user/repos" -Method Post -Headers $headers -Body $body -ContentType "application/json"
    Write-Host "  created $($r.html_url)"
} catch {
    if ($_.Exception.Response.StatusCode.value__ -eq 422) {
        Write-Host "  already exists, using it"
    } else { throw }
}

$url = "https://github.com/$($me.login)/$Repo.git"
Write-Host "== push =="
& $git -c "http.https://github.com/.extraheader=AUTHORIZATION: bearer $Token" push $url "main:main"
if ($LASTEXITCODE -ne 0) { throw "push failed" }

& $git remote remove origin 2>$null
& $git remote add origin $url
Write-Host "== done =="
Write-Host "  https://github.com/$($me.login)/$Repo"

# Secrets stay out of the repo: provide your own key via tools/secrets.local.ps1 (gitignored).
# Format of tools/secrets.local.ps1:
#   $env:OBSIDIAN_API_KEY = "<your Obsidian Local REST API key>"
if (Test-Path "$PSScriptRoot\secrets.local.ps1") {
    & "$PSScriptRoot\secrets.local.ps1"
}
if (-not $env:OBSIDIAN_API_KEY) {
    Write-Error "OBSIDIAN_API_KEY not set. Create tools/secrets.local.ps1 with your Obsidian Local REST API key."
    exit 1
}
$env:OBSIDIAN_BASE_URL = "https://127.0.0.1:27124"
$env:OBSIDIAN_VERIFY_SSL = "false"
$env:OBSIDIAN_VAULT_PATH = "$PSScriptRoot\..\docs"
& "C:\Users\aaaaaaaaa\.bun\bin\obsidian-mcp-server.exe" @args

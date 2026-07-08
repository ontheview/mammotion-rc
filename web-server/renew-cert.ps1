# renew-cert.ps1
# EXAMPLE script from one particular setup (Windows + cPanel-hosted DNS).
# Obtain / renew a Let's Encrypt certificate via DNS-01 (cPanel API) and export
# it as the cert.pem / key.pem that run-server.bat already points at
# (..\cert.pem and ..\key.pem, i.e. C:\LubaRC\ble-proxy\).
#
# DNS-01 itself is provider-agnostic -- only the Posh-ACME plugin below is
# cPanel-specific. For a different DNS host, swap `-Plugin cPanel` and $pArgs
# for your provider's plugin (Get-PAPlugin lists the options).
#
# Why DNS-01: only port 8443 is forwarded here, so Let's Encrypt can't reach
# ports 80/443 for HTTP validation. DNS-01 needs no inbound ports -- it proves
# control by writing a TXT record into the DNS zone.
#
# ---------------------------------------------------------------------------
# ONE-TIME SETUP (run once, in PowerShell, as the user that runs the server):
#   Install-Module -Name Posh-ACME -Scope CurrentUser
# Then fill in the values below and run this script once interactively. After
# that, schedule it (see bottom) so renewals are automatic.
# ---------------------------------------------------------------------------

$ErrorActionPreference = 'Stop'

# ===== EDIT THESE =====
$Domain      = 'home.example.com'              # the dynamic hostname your router updates
$Contact     = 'you@example.com'               # LE expiry-warning email
$CpanelUri   = 'https://your-vps-host:2083'    # cPanel login URL (port 2083)
$CpanelUser  = 'your_cpanel_user'
$CpanelToken = 'PASTE_API_TOKEN'               # cPanel > Security > Manage API Tokens
# ======================

Import-Module Posh-ACME
Set-PAServer LE_PROD                            # use LE_STAGE first if you want to test without rate limits

# NOTE: confirm the exact arg names your Posh-ACME version expects with:
#   Get-PAPlugin cPanel -Verbose
# (older builds used cPanelApiUrl / cPanelToken; newer ones may differ)
$pArgs = @{
    cPanelUsername = $CpanelUser
    cPanelToken    = $CpanelToken
    cPanelApiUrl   = $CpanelUri
}

$existing = Get-PACertificate $Domain -ErrorAction SilentlyContinue
if ($existing) {
    # Renews only if within the renewal window; returns $null if not yet due.
    $cert = Submit-Renewal $Domain
    if (-not $cert) {
        Write-Host "Cert not due for renewal; re-exporting current one."
        $cert = $existing
        $renewed = $false
    } else {
        $renewed = $true
    }
} else {
    $cert = New-PACertificate $Domain -Plugin cPanel -PluginArgs $pArgs `
            -AcceptTOS -Contact $Contact -Install:$false
    $renewed = $true
}

# Export to the PEM paths uvicorn loads (parent of this script's folder).
$dest = Split-Path -Parent $PSScriptRoot       # ..\  -> C:\LubaRC\ble-proxy
Copy-Item $cert.FullChainFile (Join-Path $dest 'cert.pem') -Force
Copy-Item $cert.KeyFile       (Join-Path $dest 'key.pem')  -Force
Write-Host "Cert exported to $dest"

# uvicorn does NOT hot-reload certs, so restart the server after a real renewal.
# Adapt this to however you run it. Example if you run it as a scheduled task
# named "LubaRC":
# if ($renewed) {
#     Stop-ScheduledTask  -TaskName 'LubaRC'
#     Start-ScheduledTask -TaskName 'LubaRC'
# }

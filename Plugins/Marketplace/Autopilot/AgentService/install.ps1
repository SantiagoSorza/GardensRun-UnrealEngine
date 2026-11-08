# Installs Autopilot's agent service (the `autopilot` command) from the wheel beside this script, on Windows.
#
#   powershell -ExecutionPolicy Bypass -File install.ps1
#
# It uses uv (https://docs.astral.sh/uv/), installing it first if it isn't there, and uv brings a Python 3.12 of its
# own; the agent, OpenCode, runs under Bun (https://bun.sh), installed the same way: nothing else needs to be
# installed. Run it again to update. Uninstall with `autopilot uninstall` (or Remove from this computer, on a
# plugin's Settings page).

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$settings = Join-Path $env:LOCALAPPDATA "Autopilot"
$envFile = Join-Path $settings "autopilot.env"
$wheel = Get-ChildItem -Path $here -Filter "autopilot-*.whl" | Select-Object -First 1
if (-not $wheel) { throw "No autopilot-*.whl next to install.ps1: unpack the whole download, then run it again." }

if (-not (Get-Command uv -ErrorAction SilentlyContinue)) {
    Write-Host "Installing uv (https://docs.astral.sh/uv/), which installs Autopilot and its Python..."
    powershell -ExecutionPolicy Bypass -c "irm https://astral.sh/uv/install.ps1 | iex"
    $env:Path = "$env:USERPROFILE\.local\bin;$env:Path"
}

Write-Host "Installing Autopilot from $($wheel.Name)..."
# An update never leaves a broken Autopilot behind. `uv tool install --force` deletes the installed environment first,
# and a program running from it (another editor's agent service, the `autopilot mcp` a Claude Code or Codex session
# keeps open) holds files Windows won't delete: uv then stopped halfway, Lib (the autopilot package with it) gone. A
# held folder and a running .exe can still be moved, so the installed environment and its command are moved aside,
# the new one is installed where they were and checked, and on any failure the old ones are moved back. What was moved
# aside is deleted once nothing holds it (here, or by the next install).
$toolDir = "$(uv tool dir)".Trim()
$binDir = "$(uv tool dir --bin)".Trim()
if ($LASTEXITCODE -ne 0 -or -not $binDir) { $binDir = Join-Path $env:USERPROFILE ".local\bin" }  # (an older uv)
$envDir = Join-Path $toolDir "autopilot"
$command = Join-Path $binDir "autopilot.exe"
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$asideDir = Join-Path (Split-Path -Parent $toolDir) "autopilot-previous"  # (not in the tool folder: uv lists those)
$envAside = Join-Path $asideDir "autopilot-$stamp"
$commandAside = "autopilot.exe.$stamp.old"  # (beside it: an .exe moves only within its drive)

function Remove-Aside {
    # (what earlier updates moved aside: a file still held stays for the next one)
    if (Test-Path $asideDir) {
        Get-ChildItem $asideDir | ForEach-Object { Remove-Item -Recurse -Force $_.FullName -ErrorAction SilentlyContinue }
    }
    Get-ChildItem $binDir -Filter "autopilot.exe.*.old" -ErrorAction SilentlyContinue |
        ForEach-Object { Remove-Item -Force $_.FullName -ErrorAction SilentlyContinue }
}

function Restore-Previous {
    if ($script:movedEnv) {
        if (Test-Path $envDir) { Remove-Item -Recurse -Force $envDir -ErrorAction SilentlyContinue }  # (what uv made of it)
        Move-Item -LiteralPath $envAside -Destination $envDir -ErrorAction SilentlyContinue
    }
    if ($script:movedCommand) {
        if (Test-Path $command) { Remove-Item -Force $command -ErrorAction SilentlyContinue }
        Rename-Item -LiteralPath (Join-Path $binDir $commandAside) -NewName "autopilot.exe" -ErrorAction SilentlyContinue
    }
    if ($script:movedEnv -and -not (Test-Path (Join-Path $envDir "Lib\site-packages\autopilot"))) {
        return " The version installed before couldn't be put back either: close every program using Autopilot, then run this again."
    }
    if ($script:movedEnv) { return " The version installed before is kept." }
    return ""
}

Remove-Aside
$script:movedEnv = $false
$script:movedCommand = $false
$inUse = ("Close what runs Autopilot (another editor's Autopilot tab, a Claude Code or Codex session using it), then " +
          "run this again. The version installed before is unchanged.")
if (Test-Path $envDir) {
    # (the command first: a running .exe can't be moved, so an agent service it runs, another editor's, leaves
    # everything as it was)
    if (Test-Path $command) {
        try { Rename-Item -LiteralPath $command -NewName $commandAside -ErrorAction Stop; $script:movedCommand = $true }
        catch { throw "Couldn't update Autopilot: $command is running. $inUse" }
    }
    New-Item -ItemType Directory -Force -Path $asideDir | Out-Null
    try { Move-Item -LiteralPath $envDir -Destination $envAside -ErrorAction Stop; $script:movedEnv = $true }
    catch {
        Restore-Previous | Out-Null
        throw "Couldn't update Autopilot: its installed files are in use ($($_.Exception.Message)). $inUse"
    }
}
# The wheel is installed from a copy dated now, in a folder of its own. uv keeps a local wheel it has unpacked by its
# path and date, and every download's files are dated alike (2026-01-01, tools/package_release.py), so a plugin updated
# in place would have uv install the build before it again, from its cache, and say it installed this one
$fresh = Join-Path ([System.IO.Path]::GetTempPath()) ("autopilot-wheel-" + [guid]::NewGuid().ToString("N"))
try {
    New-Item -ItemType Directory -Force -Path $fresh | Out-Null
    $copy = Join-Path $fresh $wheel.Name
    Copy-Item -LiteralPath $wheel.FullName -Destination $copy
    (Get-Item -LiteralPath $copy).LastWriteTime = Get-Date
}
catch {
    Remove-Item -Recurse -Force $fresh -ErrorAction SilentlyContinue
    $kept = Restore-Previous
    throw "Couldn't copy $($wheel.Name) to install it ($($_.Exception.Message)).$kept"
}
$uri = ([System.Uri]$copy).AbsoluteUri
uv tool install --force --python 3.12 "autopilot[claude,codex] @ $uri"
$installed = $LASTEXITCODE
Remove-Item -Recurse -Force $fresh -ErrorAction SilentlyContinue
if ($installed -eq 0) {
    & (Join-Path $envDir "Scripts\python.exe") -I -c "import autopilot.cli"
    if ($LASTEXITCODE -ne 0) {
        $kept = Restore-Previous
        throw "uv installed Autopilot, but it doesn't start (python -c `"import autopilot.cli`" failed).$kept"
    }
}
else {
    $kept = Restore-Previous
    throw "uv couldn't install Autopilot (exit code $installed).$kept"
}
Remove-Aside
uv tool update-shell | Out-Null

# Which agent service this is (agent-service.json beside this script), kept in the settings folder: a plugin carrying
# another one installs its own before it starts the agent service
$stamp = Join-Path $here "agent-service.json"
if (Test-Path $stamp) {
    New-Item -ItemType Directory -Force -Path $settings | Out-Null
    Copy-Item -Force $stamp (Join-Path $settings "agent-service.json")
}

# Settings the download carries (settings.env beside this script, e.g. which Aura server it was built for) go into
# autopilot.env over what is there; those an earlier download carried and this one doesn't are taken out
# (python -I, here and below: the installed Autopilot, whatever folder this runs in; from one holding a checkout's
# autopilot/, that was imported instead, and OpenCode's packages went into it)
$python = Join-Path (uv tool dir) "autopilot\Scripts\python.exe"
& $python -I -c "import sys; from autopilot import account; account.apply_download_settings(sys.argv[1])" (Join-Path $here "settings.env")
if ($LASTEXITCODE -ne 0) { throw "Couldn't save the download's settings in $envFile (exit code $LASTEXITCODE)." }

# The Coding Agents: Claude Code and Codex come with Autopilot; OpenCode, the default, runs under Bun (installed here when it
# isn't there), and its packages are fetched now, not at the first run. Without Bun, Claude Code is the default
$bun = Get-Command bun -ErrorAction SilentlyContinue
$bunHome = Join-Path $env:USERPROFILE ".bun\bin\bun.exe"
if (-not $bun -and -not (Test-Path $bunHome)) {
    Write-Host "Installing Bun (https://bun.sh), which runs the agent..."
    try { powershell -ExecutionPolicy Bypass -c "irm bun.sh/install.ps1 | iex" } catch { Write-Host "  $_" }
}
& $python -I -c "from autopilot.agents import opencode; opencode.ensure_installed(opencode.bun())"
if ($LASTEXITCODE -ne 0) {
    New-Item -ItemType Directory -Force -Path $settings | Out-Null
    $lines = @()
    if (Test-Path $envFile) { $lines = Get-Content $envFile }
    if (-not ($lines | Where-Object { $_ -match '^\s*(export\s+)?AUTOPILOT_AGENT\s*=' })) {
        Add-Content -Path $envFile -Value "AUTOPILOT_AGENT=claude"
    }
    Write-Host "OpenCode couldn't be set up under Bun, so Autopilot will use Claude Code; Codex works too (Coding Agent in the"
    Write-Host "first-run setup). Install Bun (https://bun.sh), then run this again to use OpenCode."
}

Write-Host ""
Write-Host "Autopilot is installed. Autopilot starts it with your editor: open the Autopilot tab and finish its first-run"
Write-Host "setup. To run it yourself instead, run this before you open the editor (Autopilot connects to it):"
Write-Host "  autopilot serve"

# SIG # Begin signature block
# MIIowQYJKoZIhvcNAQcCoIIosjCCKK4CAQExDzANBglghkgBZQMEAgEFADB5Bgor
# BgEEAYI3AgEEoGswaTA0BgorBgEEAYI3AgEeMCYCAwEAAAQQH8w7YFlLCE63JNLG
# KX7zUQIBAAIBAAIBAAIBAAIBADAxMA0GCWCGSAFlAwQCAQUABCBvPnw6hUK+eZyx
# zM/Xz2Tl1xhHIphGZQZwMNfcF4LJFqCCDfcwggawMIIEmKADAgECAhAIrUCyYNKc
# TJ9ezam9k67ZMA0GCSqGSIb3DQEBDAUAMGIxCzAJBgNVBAYTAlVTMRUwEwYDVQQK
# EwxEaWdpQ2VydCBJbmMxGTAXBgNVBAsTEHd3dy5kaWdpY2VydC5jb20xITAfBgNV
# BAMTGERpZ2lDZXJ0IFRydXN0ZWQgUm9vdCBHNDAeFw0yMTA0MjkwMDAwMDBaFw0z
# NjA0MjgyMzU5NTlaMGkxCzAJBgNVBAYTAlVTMRcwFQYDVQQKEw5EaWdpQ2VydCwg
# SW5jLjFBMD8GA1UEAxM4RGlnaUNlcnQgVHJ1c3RlZCBHNCBDb2RlIFNpZ25pbmcg
# UlNBNDA5NiBTSEEzODQgMjAyMSBDQTEwggIiMA0GCSqGSIb3DQEBAQUAA4ICDwAw
# ggIKAoICAQDVtC9C0CiteLdd1TlZG7GIQvUzjOs9gZdwxbvEhSYwn6SOaNhc9es0
# JAfhS0/TeEP0F9ce2vnS1WcaUk8OoVf8iJnBkcyBAz5NcCRks43iCH00fUyAVxJr
# Q5qZ8sU7H/Lvy0daE6ZMswEgJfMQ04uy+wjwiuCdCcBlp/qYgEk1hz1RGeiQIXhF
# LqGfLOEYwhrMxe6TSXBCMo/7xuoc82VokaJNTIIRSFJo3hC9FFdd6BgTZcV/sk+F
# LEikVoQ11vkunKoAFdE3/hoGlMJ8yOobMubKwvSnowMOdKWvObarYBLj6Na59zHh
# 3K3kGKDYwSNHR7OhD26jq22YBoMbt2pnLdK9RBqSEIGPsDsJ18ebMlrC/2pgVItJ
# wZPt4bRc4G/rJvmM1bL5OBDm6s6R9b7T+2+TYTRcvJNFKIM2KmYoX7BzzosmJQay
# g9Rc9hUZTO1i4F4z8ujo7AqnsAMrkbI2eb73rQgedaZlzLvjSFDzd5Ea/ttQokbI
# YViY9XwCFjyDKK05huzUtw1T0PhH5nUwjewwk3YUpltLXXRhTT8SkXbev1jLchAp
# QfDVxW0mdmgRQRNYmtwmKwH0iU1Z23jPgUo+QEdfyYFQc4UQIyFZYIpkVMHMIRro
# OBl8ZhzNeDhFMJlP/2NPTLuqDQhTQXxYPUez+rbsjDIJAsxsPAxWEQIDAQABo4IB
# WTCCAVUwEgYDVR0TAQH/BAgwBgEB/wIBADAdBgNVHQ4EFgQUaDfg67Y7+F8Rhvv+
# YXsIiGX0TkIwHwYDVR0jBBgwFoAU7NfjgtJxXWRM3y5nP+e6mK4cD08wDgYDVR0P
# AQH/BAQDAgGGMBMGA1UdJQQMMAoGCCsGAQUFBwMDMHcGCCsGAQUFBwEBBGswaTAk
# BggrBgEFBQcwAYYYaHR0cDovL29jc3AuZGlnaWNlcnQuY29tMEEGCCsGAQUFBzAC
# hjVodHRwOi8vY2FjZXJ0cy5kaWdpY2VydC5jb20vRGlnaUNlcnRUcnVzdGVkUm9v
# dEc0LmNydDBDBgNVHR8EPDA6MDigNqA0hjJodHRwOi8vY3JsMy5kaWdpY2VydC5j
# b20vRGlnaUNlcnRUcnVzdGVkUm9vdEc0LmNybDAcBgNVHSAEFTATMAcGBWeBDAED
# MAgGBmeBDAEEATANBgkqhkiG9w0BAQwFAAOCAgEAOiNEPY0Idu6PvDqZ01bgAhql
# +Eg08yy25nRm95RysQDKr2wwJxMSnpBEn0v9nqN8JtU3vDpdSG2V1T9J9Ce7FoFF
# UP2cvbaF4HZ+N3HLIvdaqpDP9ZNq4+sg0dVQeYiaiorBtr2hSBh+3NiAGhEZGM1h
# mYFW9snjdufE5BtfQ/g+lP92OT2e1JnPSt0o618moZVYSNUa/tcnP/2Q0XaG3Ryw
# YFzzDaju4ImhvTnhOE7abrs2nfvlIVNaw8rpavGiPttDuDPITzgUkpn13c5Ubdld
# AhQfQDN8A+KVssIhdXNSy0bYxDQcoqVLjc1vdjcshT8azibpGL6QB7BDf5WIIIJw
# 8MzK7/0pNVwfiThV9zeKiwmhywvpMRr/LhlcOXHhvpynCgbWJme3kuZOX956rEnP
# LqR0kq3bPKSchh/jwVYbKyP/j7XqiHtwa+aguv06P0WmxOgWkVKLQcBIhEuWTatE
# QOON8BUozu3xGFYHKi8QxAwIZDwzj64ojDzLj4gLDb879M4ee47vtevLt/B3E+bn
# KD+sEq6lLyJsQfmCXBVmzGwOysWGw/YmMwwHS6DTBwJqakAwSEs0qFEgu60bhQji
# WQ1tygVQK+pKHJ6l/aCnHwZ05/LWUpD9r4VIIflXO7ScA+2GRfS0YW6/aOImYIbq
# yK+p/pQd52MbOoZWeE4wggc/MIIFJ6ADAgECAhAGIHwU0816HPO5/cZvlRISMA0G
# CSqGSIb3DQEBCwUAMGkxCzAJBgNVBAYTAlVTMRcwFQYDVQQKEw5EaWdpQ2VydCwg
# SW5jLjFBMD8GA1UEAxM4RGlnaUNlcnQgVHJ1c3RlZCBHNCBDb2RlIFNpZ25pbmcg
# UlNBNDA5NiBTSEEzODQgMjAyMSBDQTEwHhcNMjUwNzAyMDAwMDAwWhcNMjgwNzAx
# MjM1OTU5WjCBxzETMBEGCysGAQQBgjc8AgEDEwJVUzEZMBcGCysGAQQBgjc8AgEC
# EwhEZWxhd2FyZTEdMBsGA1UEDwwUUHJpdmF0ZSBPcmdhbml6YXRpb24xEDAOBgNV
# BAUTBzU3NDQ1NDQxCzAJBgNVBAYTAlVTMRMwEQYDVQQIEwpDYWxpZm9ybmlhMRYw
# FAYDVQQHEw1TYW4gRnJhbmNpc2NvMRQwEgYDVQQKEwtUcmliZVZSIEluYzEUMBIG
# A1UEAxMLVHJpYmVWUiBJbmMwggGiMA0GCSqGSIb3DQEBAQUAA4IBjwAwggGKAoIB
# gQDFleYblLmsUTl8DL8MPV9qwwYyuvqgWwu7H5rzm5QHGFa9Iu24qrJ8vZUz1qy9
# Rkd4eSqxuUUH3O2pR4bZLze00avj+kbLVprNSjsd+GjalLyPBlH/Mgrg5HXb713/
# TL8YhiiGyQTb7xo6G/+PHeo/Lqv8zyDMQyMNvGhk0H3nH1CTYvQOV/iApjlfUYI5
# wc7I0E3CJP4mlM8we+RI9oPOqr0hgmIDvmG7w6xDQ55ikF1Qn3cBTbhMbNYA6V3l
# Dqajh1b8vajqWrY2MxVeLAR31yo++7//PaZCJmyveIBXYMp7P5DlyfF3LAjAsxq0
# Q/9ximGHDy6bH1Qu6lhgfpT7dK1sOh5h1dqeac/5QHPz2NAx7Pm0kFbekzlMCGJB
# afZC/o8taW0p7p6RJudGjpBbQH6Gg8ZZWqe9cI03PHva9Gp17+zvPDQZickqGC9L
# PQoLTj/mW/wu0oho+3GTwF7ERCdXrA4k0Vjqok5MXRBjxI15gYPlB8L4dzzLCy7U
# DEkCAwEAAaOCAgIwggH+MB8GA1UdIwQYMBaAFGg34Ou2O/hfEYb7/mF7CIhl9E5C
# MB0GA1UdDgQWBBQQFKtT2BNRbcQLbkqwqLkxwP+bfTA9BgNVHSAENjA0MDIGBWeB
# DAEDMCkwJwYIKwYBBQUHAgEWG2h0dHA6Ly93d3cuZGlnaWNlcnQuY29tL0NQUzAO
# BgNVHQ8BAf8EBAMCB4AwEwYDVR0lBAwwCgYIKwYBBQUHAwMwgbUGA1UdHwSBrTCB
# qjBToFGgT4ZNaHR0cDovL2NybDMuZGlnaWNlcnQuY29tL0RpZ2lDZXJ0VHJ1c3Rl
# ZEc0Q29kZVNpZ25pbmdSU0E0MDk2U0hBMzg0MjAyMUNBMS5jcmwwU6BRoE+GTWh0
# dHA6Ly9jcmw0LmRpZ2ljZXJ0LmNvbS9EaWdpQ2VydFRydXN0ZWRHNENvZGVTaWdu
# aW5nUlNBNDA5NlNIQTM4NDIwMjFDQTEuY3JsMIGUBggrBgEFBQcBAQSBhzCBhDAk
# BggrBgEFBQcwAYYYaHR0cDovL29jc3AuZGlnaWNlcnQuY29tMFwGCCsGAQUFBzAC
# hlBodHRwOi8vY2FjZXJ0cy5kaWdpY2VydC5jb20vRGlnaUNlcnRUcnVzdGVkRzRD
# b2RlU2lnbmluZ1JTQTQwOTZTSEEzODQyMDIxQ0ExLmNydDAJBgNVHRMEAjAAMA0G
# CSqGSIb3DQEBCwUAA4ICAQB63u0SCSWjXLD+VMsubxnqiJXzxB5QA+LoQBf2/1I9
# YMMWWJmAbXphMwPJ2uGTOhB/pJG2pJxqAFqqEiYn6j34bbKPLds9iX7qK27aOiY7
# PDY2RrwJkbccM7zgVxoWUhSbsgLuSl5Yxrk+JBDJpt1wUcA34cV1F3EV0ZTXSCY6
# iyWpdFX8KM6NHg3OG/h78XFGqbgy6s8idB7W5HtxfMIeHIc3Jop/0ahugbDUsCXy
# TDnsn5tmDbanhUpS3ct8NkQSpXnGswFOblyOQu4vj3yfimdhJGskBlf/BviyvWpD
# r67OpN+D8/k7UIAqFqHzuh7Z1l+OXLokuQALXuvSWcDqeewuJQQVIVq3PALnMSmi
# Nk+cPHFetqA1H7m7rJgF4WxemoElVO8I2f+FFpsfhC6GHz6UR2QQC2N7Jb9iUkeo
# lKyQnmqn7J0B7LFhxHeMivCYmF6GsES5l2bnty36oBAPSdULVkjuBvSqUKgyS2Aw
# W9Gtgu8MD/ZQ1SN6W/RAkACuTn3pego6jiYCbR62x+agdDYG+lZQISGPcw+EcyCe
# PkB4ZvljdCB6T8vcf20aY9OMGaq8sFCdEL1/vsHdNb/n0rJ0vRpQHIBrsqBsoP+W
# 6QUMr0o78gueK4z4L/mGdLKtsfRlfm+HJ06+EuMuvItmhxWtliiAupSOik8qHOdF
# JjGCGiAwghocAgEBMH0waTELMAkGA1UEBhMCVVMxFzAVBgNVBAoTDkRpZ2lDZXJ0
# LCBJbmMuMUEwPwYDVQQDEzhEaWdpQ2VydCBUcnVzdGVkIEc0IENvZGUgU2lnbmlu
# ZyBSU0E0MDk2IFNIQTM4NCAyMDIxIENBMQIQBiB8FNPNehzzuf3Gb5USEjANBglg
# hkgBZQMEAgEFAKB8MBAGCisGAQQBgjcCAQwxAjAAMBkGCSqGSIb3DQEJAzEMBgor
# BgEEAYI3AgEEMBwGCisGAQQBgjcCAQsxDjAMBgorBgEEAYI3AgEVMC8GCSqGSIb3
# DQEJBDEiBCBQa7n65Zt8jOE2UGrbB3JXRvE1GxcpMNDxNW99FUi8HjANBgkqhkiG
# 9w0BAQEFAASCAYBOIf4NVgPqftTXGWbsWaiW5peih64KuxNr/AIXfoX6phavzN8c
# gOCt+Gu6Kd97DCcbYtMXrj4bp4Psek0bQ5h3DqGc9LfTkE0JRsQdZ9lUTbYe93H+
# jwNd5tmFRboNqUsKUlqc03/xM8mBhWpmvOyNRpGvfu3egyga/YepM8B/e1H+07eE
# OhjCrfKuoF3DSnk+2Ma7GcTUkyur5EPhByMUqs1pzDG7D1ir5zUE/WEJ1iThrxaj
# MDwOD6Qvt1+FGF8hBP6UU1dstzX+vG/Hc61O8Lk9FQO1XWuCHuVRlijsG5vg+vNd
# o/OWRTTa7oGk0bwrjllQxBHoTIRUtz3ahGhLP3H1FOS9nn/MYA/BuJeCecugSfNH
# 3QrfIuqWe9XHSuIf5U/RCxvJCCGK4EdZqtDF334pa3dogKsMTGwI47C+A+LEkByR
# T/5TNsXrcvZ4lh5uF+pXuJ/Ubrf5MUzZ1lk6OYygAX7OYw7xOlmho2qKcbW+iq7G
# hMnvkJjrEFwEIPqhghd2MIIXcgYKKwYBBAGCNwMDATGCF2IwghdeBgkqhkiG9w0B
# BwKgghdPMIIXSwIBAzEPMA0GCWCGSAFlAwQCAQUAMHcGCyqGSIb3DQEJEAEEoGgE
# ZjBkAgEBBglghkgBhv1sBwEwMTANBglghkgBZQMEAgEFAAQgoJK6DYeSFXLFIU+v
# m9xPiHDAt/3uUkR8/OD4uyVTEgYCEHi34cEQLA1ixhigRHyMIPUYDzIwMjYxMTA0
# MDI0OTMxWqCCEzowggbtMIIE1aADAgECAhAIT9wzT35FTtvDD4/5khg1MA0GCSqG
# SIb3DQEBCwUAMGkxCzAJBgNVBAYTAlVTMRcwFQYDVQQKEw5EaWdpQ2VydCwgSW5j
# LjFBMD8GA1UEAxM4RGlnaUNlcnQgVHJ1c3RlZCBHNCBUaW1lU3RhbXBpbmcgUlNB
# NDA5NiBTSEEyNTYgMjAyNSBDQTEwHhcNMjYwODA1MDAwMDAwWhcNMzcxMTA0MjM1
# OTU5WjBjMQswCQYDVQQGEwJVUzEXMBUGA1UEChMORGlnaUNlcnQsIEluYy4xOzA5
# BgNVBAMTMkRpZ2lDZXJ0IFNIQTI1NiBSU0E0MDk2IFRpbWVzdGFtcCBSZXNwb25k
# ZXIgMjAyNiAxMIICIjANBgkqhkiG9w0BAQEFAAOCAg8AMIICCgKCAgEAtnum8sn+
# zUr41JtMZbP9OMYw+HwJDpG5xkIu/lqcfNYmMX81YmsUiHLbh9ykpeWBGKTLhYBr
# AN9Tdg/QEzG32XcObmgIblnr0CoQ3WSAeDZ6nH6X6VkFyYkJw3QBJREwvm4UhLzS
# xmwPA7cFKRTEOMsmEEj6qJk/dqLEAL+oQYuOwE2UuiX1Vnul8YReIyWd4kgLn9gq
# 6LNXM0UplkR6jL/QHxmb6fMoGBJYbnaUI7XD6cKDpekK2SVMld4iDbzeHDtOaaxl
# dH5IxuNusQ69nd8/ZXEiB5Hbxj3RlK13cX1W4DlFXKdv/CEhM8Cj1vvlmvhNroyP
# dRGbbpBlgyf8Wdu5N6ByhFwURn0U6ozlPoxN22v+fviUhP+6DR547OZnpBMWDfei
# 1f5sVGwiiW/KQTWOK97g+4RJpPzPNV4VYMAwO2jM2Aty2QYPVmOQTJm0msuXnJrS
# bl2gf9JylpkJlWXqk1Q4LJsxz+TELoQCZIljbgvTJgoPU2R12ydv8i1UqL/adelA
# 0y7U9Pmmtbze9Xx3rtajC5SzQd1jgfwAwsa90v9YcSPdmeoyoBBA/27cCL237l5D
# TYYPDLQ4ON3OLTGWnvRb6jDrf/T75gMRfUzSLCBQfBusm9+mSWRlC/Df6S/e9Q8i
# 13CuhzOT2Jx+V/nlbXM4QoBwlUAhelwwJT0CAwEAAaOCAZUwggGRMAwGA1UdEwEB
# /wQCMAAwHQYDVR0OBBYEFBTJY4owLtRK+26U8+bjQH717M3iMB8GA1UdIwQYMBaA
# FO9vU0rp5AZ8esrikFb2L9RJ7MtOMA4GA1UdDwEB/wQEAwIHgDAWBgNVHSUBAf8E
# DDAKBggrBgEFBQcDCDCBlQYIKwYBBQUHAQEEgYgwgYUwJAYIKwYBBQUHMAGGGGh0
# dHA6Ly9vY3NwLmRpZ2ljZXJ0LmNvbTBdBggrBgEFBQcwAoZRaHR0cDovL2NhY2Vy
# dHMuZGlnaWNlcnQuY29tL0RpZ2lDZXJ0VHJ1c3RlZEc0VGltZVN0YW1waW5nUlNB
# NDA5NlNIQTI1NjIwMjVDQTEuY3J0MF8GA1UdHwRYMFYwVKBSoFCGTmh0dHA6Ly9j
# cmwzLmRpZ2ljZXJ0LmNvbS9EaWdpQ2VydFRydXN0ZWRHNFRpbWVTdGFtcGluZ1JT
# QTQwOTZTSEEyNTYyMDI1Q0ExLmNybDAgBgNVHSAEGTAXMAgGBmeBDAEEAjALBglg
# hkgBhv1sBwEwDQYJKoZIhvcNAQELBQADggIBAI3FOmEenVIK35msCYB+fShAsWvS
# YvLBItoNdAgQ2jIqrGsVsluXMJU/+mRebBc52s6lbKAvOVPXaizmKkMLLflEEKDZ
# Qx4CkS2t8aHPjkXha3hYZ010htFa3dhNgmalH5vuWvh3tTCf4frTS7gPtGc4Z/xa
# PhQ2AB1mR8eEe/WbH0RWHvVIl6VwQ3+g5FKNfN2N/DWJkf13w2H+2GfqEfbd35Ww
# 8CvoYBjLNIDTadcPWdgsjsiOaK/7EsKJgLjUNIVgvcaFOLLQ/GlrA+0ZHJoFUbOr
# 5SJN8zykPspXIXlpDJY/gqFUZRROeab9GVgmhbdOJcD/63RhxPahFUGbckRONqMe
# 6DYAv6/mOG0pWd3cPStsdcS7buj5DyniwRY8yooMH6ptx5vpP/pZzBPBeZD2U4Is
# thyxB5Jaa8qrOkB5z160TXiM5ADMspZ0TfD9MJoq0tFpFPssKRFhWeEDYPvcUuN7
# U7lvcdHl4ezQ3NT/7Ffs1sR1yh/LRbdZ3B3Vc6q2WmD8mDC0p9kzl2o73iVtS946
# IkEj7FkRsZGww1teYxERROC745xrtjvcw9ZyyUjHZWGRIpJeMNsPquCDf0fkyHtB
# +J4AiNZqCQk23rxh+KbpyMTNVKItJ5l92Svl20U9NbqMBOVYl1h54NEYLJq1/xHW
# FKPNK903zJZA9P2DMIIGtDCCBJygAwIBAgIQDcesVwX/IZkuQEMiDDpJhjANBgkq
# hkiG9w0BAQsFADBiMQswCQYDVQQGEwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5j
# MRkwFwYDVQQLExB3d3cuZGlnaWNlcnQuY29tMSEwHwYDVQQDExhEaWdpQ2VydCBU
# cnVzdGVkIFJvb3QgRzQwHhcNMjUwNTA3MDAwMDAwWhcNMzgwMTE0MjM1OTU5WjBp
# MQswCQYDVQQGEwJVUzEXMBUGA1UEChMORGlnaUNlcnQsIEluYy4xQTA/BgNVBAMT
# OERpZ2lDZXJ0IFRydXN0ZWQgRzQgVGltZVN0YW1waW5nIFJTQTQwOTYgU0hBMjU2
# IDIwMjUgQ0ExMIICIjANBgkqhkiG9w0BAQEFAAOCAg8AMIICCgKCAgEAtHgx0wqY
# QXK+PEbAHKx126NGaHS0URedTa2NDZS1mZaDLFTtQ2oRjzUXMmxCqvkbsDpz4aH+
# qbxeLho8I6jY3xL1IusLopuW2qftJYJaDNs1+JH7Z+QdSKWM06qchUP+AbdJgMQB
# 3h2DZ0Mal5kYp77jYMVQXSZH++0trj6Ao+xh/AS7sQRuQL37QXbDhAktVJMQbzIB
# HYJBYgzWIjk8eDrYhXDEpKk7RdoX0M980EpLtlrNyHw0Xm+nt5pnYJU3Gmq6bNMI
# 1I7Gb5IBZK4ivbVCiZv7PNBYqHEpNVWC2ZQ8BbfnFRQVESYOszFI2Wv82wnJRfN2
# 0VRS3hpLgIR4hjzL0hpoYGk81coWJ+KdPvMvaB0WkE/2qHxJ0ucS638ZxqU14lDn
# ki7CcoKCz6eum5A19WZQHkqUJfdkDjHkccpL6uoG8pbF0LJAQQZxst7VvwDDjAmS
# FTUms+wV/FbWBqi7fTJnjq3hj0XbQcd8hjj/q8d6ylgxCZSKi17yVp2NL+cnT6To
# y+rN+nM8M7LnLqCrO2JP3oW//1sfuZDKiDEb1AQ8es9Xr/u6bDTnYCTKIsDq1Btm
# XUqEG1NqzJKS4kOmxkYp2WyODi7vQTCBZtVFJfVZ3j7OgWmnhFr4yUozZtqgPrHR
# VHhGNKlYzyjlroPxul+bgIspzOwbtmsgY1MCAwEAAaOCAV0wggFZMBIGA1UdEwEB
# /wQIMAYBAf8CAQAwHQYDVR0OBBYEFO9vU0rp5AZ8esrikFb2L9RJ7MtOMB8GA1Ud
# IwQYMBaAFOzX44LScV1kTN8uZz/nupiuHA9PMA4GA1UdDwEB/wQEAwIBhjATBgNV
# HSUEDDAKBggrBgEFBQcDCDB3BggrBgEFBQcBAQRrMGkwJAYIKwYBBQUHMAGGGGh0
# dHA6Ly9vY3NwLmRpZ2ljZXJ0LmNvbTBBBggrBgEFBQcwAoY1aHR0cDovL2NhY2Vy
# dHMuZGlnaWNlcnQuY29tL0RpZ2lDZXJ0VHJ1c3RlZFJvb3RHNC5jcnQwQwYDVR0f
# BDwwOjA4oDagNIYyaHR0cDovL2NybDMuZGlnaWNlcnQuY29tL0RpZ2lDZXJ0VHJ1
# c3RlZFJvb3RHNC5jcmwwIAYDVR0gBBkwFzAIBgZngQwBBAIwCwYJYIZIAYb9bAcB
# MA0GCSqGSIb3DQEBCwUAA4ICAQAXzvsWgBz+Bz0RdnEwvb4LyLU0pn/N0IfFiBow
# f0/Dm1wGc/Do7oVMY2mhXZXjDNJQa8j00DNqhCT3t+s8G0iP5kvN2n7Jd2E4/iEI
# UBO41P5F448rSYJ59Ib61eoalhnd6ywFLerycvZTAz40y8S4F3/a+Z1jEMK/DMm/
# axFSgoR8n6c3nuZB9BfBwAQYK9FHaoq2e26MHvVY9gCDA/JYsq7pGdogP8HRtrYf
# ctSLANEBfHU16r3J05qX3kId+ZOczgj5kjatVB+NdADVZKON/gnZruMvNYY2o1f4
# MXRJDMdTSlOLh0HCn2cQLwQCqjFbqrXuvTPSegOOzr4EWj7PtspIHBldNE2K9i69
# 7cvaiIo2p61Ed2p8xMJb82Yosn0z4y25xUbI7GIN/TpVfHIqQ6Ku/qjTY6hc3hsX
# MrS+U0yy+GWqAXam4ToWd2UQ1KYT70kZjE4YtL8Pbzg0c1ugMZyZZd/BdHLiRu7h
# AWE6bTEm4XYRkA6Tl4KSFLFk43esaUeqGkH/wyW4N7OigizwJWeukcyIPbAvjSab
# nf7+Pu0VrFgoiovRDiyx3zEdmcif/sYQsfch28bZeUz2rtY/9TCA6TD8dC3JE3rY
# krhLULy7Dc90G6e8BlqmyIjlgp2+VqsS9/wQD7yFylIz0scmbKvFoW2jNrbM1pD2
# T7m3XDCCBY0wggR1oAMCAQICEA6bGI750C3n79tQ4ghAGFowDQYJKoZIhvcNAQEM
# BQAwZTELMAkGA1UEBhMCVVMxFTATBgNVBAoTDERpZ2lDZXJ0IEluYzEZMBcGA1UE
# CxMQd3d3LmRpZ2ljZXJ0LmNvbTEkMCIGA1UEAxMbRGlnaUNlcnQgQXNzdXJlZCBJ
# RCBSb290IENBMB4XDTIyMDgwMTAwMDAwMFoXDTMxMTEwOTIzNTk1OVowYjELMAkG
# A1UEBhMCVVMxFTATBgNVBAoTDERpZ2lDZXJ0IEluYzEZMBcGA1UECxMQd3d3LmRp
# Z2ljZXJ0LmNvbTEhMB8GA1UEAxMYRGlnaUNlcnQgVHJ1c3RlZCBSb290IEc0MIIC
# IjANBgkqhkiG9w0BAQEFAAOCAg8AMIICCgKCAgEAv+aQc2jeu+RdSjwwIjBpM+zC
# pyUuySE98orYWcLhKac9WKt2ms2uexuEDcQwH/MbpDgW61bGl20dq7J58soR0uRf
# 1gU8Ug9SH8aeFaV+vp+pVxZZVXKvaJNwwrK6dZlqczKU0RBEEC7fgvMHhOZ0O21x
# 4i0MG+4g1ckgHWMpLc7sXk7Ik/ghYZs06wXGXuxbGrzryc/NrDRAX7F6Zu53yEio
# ZldXn1RYjgwrt0+nMNlW7sp7XeOtyU9e5TXnMcvak17cjo+A2raRmECQecN4x7ax
# xLVqGDgDEI3Y1DekLgV9iPWCPhCRcKtVgkEy19sEcypukQF8IUzUvK4bA3VdeGbZ
# OjFEmjNAvwjXWkmkwuapoGfdpCe8oU85tRFYF/ckXEaPZPfBaYh2mHY9WV1CdoeJ
# l2l6SPDgohIbZpp0yt5LHucOY67m1O+SkjqePdwA5EUlibaaRBkrfsCUtNJhbesz
# 2cXfSwQAzH0clcOP9yGyshG3u3/y1YxwLEFgqrFjGESVGnZifvaAsPvoZKYz0YkH
# 4b235kOkGLimdwHhD5QMIR2yVCkliWzlDlJRR3S+Jqy2QXXeeqxfjT/JvNNBERJb
# 5RBQ6zHFynIWIgnffEx1P2PsIV/EIFFrb7GrhotPwtZFX50g/KEexcCPorF+CiaZ
# 9eRpL5gdLfXZqbId5RsCAwEAAaOCATowggE2MA8GA1UdEwEB/wQFMAMBAf8wHQYD
# VR0OBBYEFOzX44LScV1kTN8uZz/nupiuHA9PMB8GA1UdIwQYMBaAFEXroq/0ksuC
# MS1Ri6enIZ3zbcgPMA4GA1UdDwEB/wQEAwIBhjB5BggrBgEFBQcBAQRtMGswJAYI
# KwYBBQUHMAGGGGh0dHA6Ly9vY3NwLmRpZ2ljZXJ0LmNvbTBDBggrBgEFBQcwAoY3
# aHR0cDovL2NhY2VydHMuZGlnaWNlcnQuY29tL0RpZ2lDZXJ0QXNzdXJlZElEUm9v
# dENBLmNydDBFBgNVHR8EPjA8MDqgOKA2hjRodHRwOi8vY3JsMy5kaWdpY2VydC5j
# b20vRGlnaUNlcnRBc3N1cmVkSURSb290Q0EuY3JsMBEGA1UdIAQKMAgwBgYEVR0g
# ADANBgkqhkiG9w0BAQwFAAOCAQEAcKC/Q1xV5zhfoKN0Gz22Ftf3v1cHvZqsoYcs
# 7IVeqRq7IviHGmlUIu2kiHdtvRoU9BNKei8ttzjv9P+Aufih9/Jy3iS8UgPITtAq
# 3votVs/59PesMHqai7Je1M/RQ0SbQyHrlnKhSLSZy51PpwYDE3cnRNTnf+hZqPC/
# Lwum6fI0POz3A8eHqNJMQBk1RmppVLC4oVaO7KTVPeix3P0c2PR3WlxUjG/voVA9
# /HYJaISfb8rbII01YBwCA8sgsKxYoA5AY8WYIsGyWfVVa88nq2x2zm8jLfR+cWoj
# ayL/ErhULSd+2DrZ8LaHlv1b0VysGMNNn3O3AamfV6peKOK5lDGCA3wwggN4AgEB
# MH0waTELMAkGA1UEBhMCVVMxFzAVBgNVBAoTDkRpZ2lDZXJ0LCBJbmMuMUEwPwYD
# VQQDEzhEaWdpQ2VydCBUcnVzdGVkIEc0IFRpbWVTdGFtcGluZyBSU0E0MDk2IFNI
# QTI1NiAyMDI1IENBMQIQCE/cM09+RU7bww+P+ZIYNTANBglghkgBZQMEAgEFAKCB
# 0TAaBgkqhkiG9w0BCQMxDQYLKoZIhvcNAQkQAQQwHAYJKoZIhvcNAQkFMQ8XDTI2
# MTEwNDAyNDkzMVowKwYLKoZIhvcNAQkQAgwxHDAaMBgwFgQUUdmr2gNJc9hPQmas
# pIJI5rNpxDkwLwYJKoZIhvcNAQkEMSIEIMtA3YSsAWLAKyVko4TGvl1PbOq8//6e
# 4aFMd5Ma83BpMDcGCyqGSIb3DQEJEAIvMSgwJjAkMCIEIC2gnaf0Ex+f5y22xebp
# yWVnVa8EPx6nQswNISDhQev8MA0GCSqGSIb3DQEBAQUABIICAJHwQhWTE4PXiZI1
# XW8ujev14jYLdfRehDCiVUuAYGfxJerf/2/YFDl0KE+F9dtnNjpzcYl4ZIenarXG
# HIE1pMlS2DAxFRx44dM20AAp9+89CPBj+ZIob4HY1Dt0BGq3JKrjKSvjfrniAR9y
# VuQEWiA7PsqO0Y7m8AUuGR1sZdcW1o9I5VU/ntDKJ0Go9GJJt4cMhH1M6vYjFV5w
# 0UPztheV89btNtNEV/WXGon4jsABTq3D4/SWBGOCi4HR5QfN+OX5TsuOLKScAXh7
# EFk1KqmKGrtEE2bIIZ6LtW5txAeRlz/jbi+P+9lroWhaPPgL9SN31KiubOmxDYH8
# z2LJjKfyLpdlCd0n7YJxEqQyew1CddDlF+EEXUMl9qhACZWRmbTP68HUQ1C81Mvl
# qWkQTFL9dlTdKRLa6b/+g04sMlBAMSKu2+SbN5RWoTgGiSZ98ldMFHJ1u8F7CNFV
# mSZP2xk9RxFL1n1vRHRCG98bdXBgzQzfDSShQsuzR5qC57HAVGV6l62bBd7Y6MRH
# iL7DZBlQY5i7KyMI964RczJue9DJLIvflaB/EkQbY1a7FxvkfaElRaZ9lWosz0DL
# lnJRI4MAcc11XZdNZtSCKOgtp8hApxRdjq9G2a2yjB81DBzIgVsQHiW+SvvVoBK6
# juJTNo0hBST7OwaYA61ZhNJrP3i9
# SIG # End signature block

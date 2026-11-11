# Lets Ember through Windows Firewall, or takes its rules away again.
#   .\ember-firewall.ps1                        add the rules for the Ember folder this script finds
#   .\ember-firewall.ps1 -Folder 'D:\Ember'     add them for that Ember folder
#   .\ember-firewall.ps1 -Remove                remove every rule this script added
# It can be run as often as you like: a rule it already added is replaced, never doubled. Every rule sits in the
# group "Ember", and that group is all -Remove touches, so rules you made yourself are left alone.
# Windows asks for administrator rights once, and only when there is something to change.
#   Matches      : sf4-net.exe, inbound, Private and Public networks. The rule is for the program, not for its
#                  ports (UDP 45760-45763, rust/sf4-net/src/transport/fixed_port.rs), so nothing else on the PC
#                  is let in through them.
#   Phone remote : TCP 47321-47328 in, private networks only, and only from the local subnet.
param([switch]$Remove, [string]$Folder)
$ErrorActionPreference = 'Stop'
$group = 'Ember'
$matchProgram = 'sf4-net.exe'

function Test-Administrator {
    ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

# The folder that holds sf4-net.exe: the one asked for, else the one this script sits in (a package folder),
# else the one Setup recorded for this user.
function Find-EmberFolder {
    $candidates = @($Folder, $PSScriptRoot)
    $setup = Get-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\{791CAF8E-B4B5-45D5-AB5C-B4864DDB8866}_is1' -ErrorAction SilentlyContinue
    if ($setup) { $candidates += $setup.InstallLocation }
    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path -LiteralPath (Join-Path $candidate $matchProgram) -PathType Leaf)) { return (Resolve-Path -LiteralPath $candidate).Path }
    }
    throw "Could not find $matchProgram. Run this script again with -Folder set to your Ember folder, for example: .\ember-firewall.ps1 -Folder 'C:\Ember'"
}

function Write-EmberRules {
    $rules = @(Get-NetFirewallRule -Group $group -ErrorAction SilentlyContinue)
    if (!$rules) { Write-Host 'No Ember rules are in Windows Firewall.'; return }
    foreach ($rule in $rules) { Write-Host "  $($rule.DisplayName) ($($rule.Direction), $($rule.Profile))" }
}

# Replaces a rule of ours with the same name, so a second run leaves one copy.
function Set-EmberRule([string]$DisplayName, [hashtable]$Rule) {
    Get-NetFirewallRule -Group $group -DisplayName $DisplayName -ErrorAction SilentlyContinue | Remove-NetFirewallRule
    New-NetFirewallRule -DisplayName $DisplayName -Group $group -Action Allow @Rule | Out-Null
    Write-Host "Added: $DisplayName"
}

if ($Remove) {
    if (!(Get-NetFirewallRule -Group $group -ErrorAction SilentlyContinue)) { Write-Host 'No Ember rules are in Windows Firewall, so there is nothing to remove.'; return }
} else {
    $Folder = Find-EmberFolder
}

if (!(Test-Administrator)) {
    # Looking around needed no rights; changing the firewall does. Ask once, in a copy of this script.
    Write-Host 'Windows will ask for administrator rights to change the firewall.'
    $arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"")
    if ($Remove) { $arguments += '-Remove' } else { $arguments += @('-Folder', "`"$Folder`"") }
    try { $child = Start-Process -FilePath 'powershell.exe' -ArgumentList $arguments -Verb RunAs -Wait -PassThru }
    catch { throw 'The firewall was not changed: administrator rights were not given.' }
    if ($child.ExitCode -ne 0) { throw "The firewall was not changed: the administrator copy of this script stopped with code $($child.ExitCode)." }
    Write-Host 'Ember rules now in Windows Firewall:'
    Write-EmberRules
    return
}

if ($Remove) {
    Get-NetFirewallRule -Group $group | Remove-NetFirewallRule
    Write-Host 'Removed every rule this script added. Rules you made yourself were not touched.'
    return
}

Set-EmberRule 'Ember matches (sf4-net.exe)' @{ Direction = 'Inbound'; Program = (Join-Path $Folder $matchProgram); Profile = @('Private','Public') }
Set-EmberRule 'Ember phone remote' @{ Direction = 'Inbound'; Protocol = 'TCP'; LocalPort = '47321-47328'; RemoteAddress = 'LocalSubnet'; Profile = 'Private' }
Write-Host "Ember is allowed through Windows Firewall for $Folder."

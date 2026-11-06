# Allows the phone remote through Windows Firewall, or takes the rules away again. Needs an administrator PowerShell.
#   .\ember-firewall.ps1            add the rules
#   .\ember-firewall.ps1 -Remove    remove every rule this script added
# The rules sit in the group "Ember", so -Remove touches nothing else.
#   Phone remote : TCP 47321-47328 in, private networks only, and only from the local subnet.
param([switch]$Remove)
$ErrorActionPreference = 'Stop'
$group = 'Ember'
Get-NetFirewallRule -Group $group -ErrorAction SilentlyContinue | Remove-NetFirewallRule
if ($Remove) { return }
New-NetFirewallRule -DisplayName 'Ember phone remote' -Group $group -Direction Inbound -Protocol TCP -LocalPort 47321-47328 `
    -RemoteAddress LocalSubnet -Profile Private -Action Allow | Out-Null

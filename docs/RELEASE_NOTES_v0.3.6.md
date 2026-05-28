# SF4 Netplay Launcher v0.3.6 (testing)

**Pre-release for testing** — supersedes v0.3.5. **v0.3.1** remains the default stable download until Authenticode signing ships.

## Install

1. Download **`sf4-netplay-launcher-*-v0.3.6.zip`** (Assets).
2. Extract on **both** PCs; run **`preflight.cmd`**, then **`Launcher.exe`**.
3. Confirm **v0.3.6** in the launcher header.

## What's fixed in v0.3.6

- **Rematch UDP drop:** VPS GGPO relay re-binds player slots on re-registration (rematch no longer times out when both slots were full).
- **Rematch transport persistence:** broker UDP relay endpoint is preserved after legacy fallback so match 3+ still retries UDP.

Includes all v0.3.5 GGPO relay fixes.

## Verify

After rematch in the same room, `sf4e.log` should show `UDP relay registration OK` again (not timeout).

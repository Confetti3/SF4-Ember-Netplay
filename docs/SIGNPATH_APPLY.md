# SignPath Foundation application checklist

> **Legacy setup record.** Ember releases use the guarded local process in [BUILDING.md](BUILDING.md). This checklist describes the old launcher's GitHub workflow and does not establish signing for Ember 0.8.0.

Complete these steps to enable Authenticode signing for release builds.

## 1. Apply

- [ ] Submit application: [signpath.org/apply](https://signpath.org/apply)
- [ ] Project policy file: [`.signpath/signpath.json`](../.signpath/signpath.json)
- [ ] Public GitHub repo, MIT license, active maintenance

## 2. Configure SignPath.io

- [ ] Install SignPath GitHub App on `Confetti3/SF4-Netplay-Launcher`
- [ ] Create project **SF4-Netplay-Launcher** (slug must match `SIGNPATH_PROJECT_SLUG` secret)
- [ ] Enable signing policy **release** (matches `.signpath/signpath.json`)

## 3. GitHub repository secrets

| Secret | Value |
|--------|--------|
| `SIGNPATH_API_TOKEN` | API token with submitter role |
| `SIGNPATH_ORGANIZATION_ID` | Organization GUID from SignPath |
| `SIGNPATH_PROJECT_SLUG` | e.g. `SF4-Netplay-Launcher` |
| `SIGNPATH_SIGNING_POLICY_SLUG` | `release` |

## 4. Release a signed build

Tag and push the version you want signed (example: current tree `v0.6.5`):

```powershell
git tag v0.6.5
git push origin v0.6.5
```

Workflow [`.github/workflows/release-windows.yml`](../.github/workflows/release-windows.yml) builds, signs via SignPath (when secrets are set), verifies Authenticode, and packages the zip.

## 5. Publish and promote

- [ ] Attach zip from CI artifacts to GitHub Release
- [ ] Post SHA256 hashes in release notes
- [ ] Run `scripts/prepare-defender-submission.ps1` → [Microsoft WDSI](https://www.microsoft.com/en-us/wdsi/filesubmission)
- [ ] `gh release edit v0.6.5 --latest` (or the tag you just published)

## 6. After signing works

Keep unsigned test builds as **pre-release** when practical. Prefer a verified signed build as GitHub **Latest**.

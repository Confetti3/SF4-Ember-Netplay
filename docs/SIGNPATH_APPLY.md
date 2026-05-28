# SignPath Foundation application checklist

Complete these steps **before** tagging **v0.3.4** as Latest.

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

## 4. Release v0.3.4

```powershell
git tag v0.3.4
git push origin v0.3.4
```

Workflow [`.github/workflows/release-windows.yml`](../.github/workflows/release-windows.yml) builds, signs via SignPath, verifies Authenticode, packages zip.

## 5. Publish and promote

- [ ] Attach zip from CI artifacts to GitHub Release
- [ ] Post SHA256 hashes in release notes
- [ ] Run `scripts/prepare-defender-submission.ps1` → [Microsoft WDSI](https://www.microsoft.com/en-us/wdsi/filesubmission)
- [ ] `gh release edit v0.3.4 --latest`

## 6. Deprecate unsigned Latest

Keep **v0.3.2** and **v0.3.3** as pre-release. **v0.3.1** remains fallback until v0.3.4 is verified signed.

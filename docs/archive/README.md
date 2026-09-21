# Branch archive

These snapshots preserve the 12 retired branches at their exact commits. They are stored as `archive/` tags so the branch selector stays focused on `release` and legacy `main`.

Select **Browse snapshot** to view the complete source and history for a retired branch.

| Former branch | Archived snapshot | Preserved commit |
| --- | --- | --- |
| `codex/v0.6.2-release` | [Browse snapshot](https://github.com/Confetti3/SF4-Ember-Netplay/tree/archive/codex/v0.6.2-release) | [`4996222`](https://github.com/Confetti3/SF4-Ember-Netplay/commit/4996222fb71b930cb827b3b52686a7c16ac7913b) |
| `codex/v0.6.3-direct-ip-launcher` | [Browse snapshot](https://github.com/Confetti3/SF4-Ember-Netplay/tree/archive/codex/v0.6.3-direct-ip-launcher) | [`8582532`](https://github.com/Confetti3/SF4-Ember-Netplay/commit/85825328982be14be049936b014da5e58b884fba) |
| `dependabot/github_actions/actions/checkout-7` | [Browse snapshot](https://github.com/Confetti3/SF4-Ember-Netplay/tree/archive/dependabot/github_actions/actions/checkout-7) | [`692b2cc`](https://github.com/Confetti3/SF4-Ember-Netplay/commit/692b2ccdc08041b1d87cef8eeb6cca85845ee275) |
| `dependabot/github_actions/actions/upload-artifact-7` | [Browse snapshot](https://github.com/Confetti3/SF4-Ember-Netplay/tree/archive/dependabot/github_actions/actions/upload-artifact-7) | [`b9077c9`](https://github.com/Confetti3/SF4-Ember-Netplay/commit/b9077c9c83ae78325040e2ccc5469fa313689873) |
| `dependabot/github_actions/azure/artifact-signing-action-2` | [Browse snapshot](https://github.com/Confetti3/SF4-Ember-Netplay/tree/archive/dependabot/github_actions/azure/artifact-signing-action-2) | [`f67b280`](https://github.com/Confetti3/SF4-Ember-Netplay/commit/f67b28064a067b5f9099b66b4e2a58d072f29f10) |
| `dependabot/github_actions/azure/login-3` | [Browse snapshot](https://github.com/Confetti3/SF4-Ember-Netplay/tree/archive/dependabot/github_actions/azure/login-3) | [`82b4d49`](https://github.com/Confetti3/SF4-Ember-Netplay/commit/82b4d49e4a88bd142eec2802a034edea649d98e8) |
| `docs/v0.6.6-rc1-notes` | [Browse snapshot](https://github.com/Confetti3/SF4-Ember-Netplay/tree/archive/docs/v0.6.6-rc1-notes) | [`c6f2f19`](https://github.com/Confetti3/SF4-Ember-Netplay/commit/c6f2f1907f396c40a8542cc056e3bafa2adb3bde) |
| `feat/async-ui-and-update-verification` | [Browse snapshot](https://github.com/Confetti3/SF4-Ember-Netplay/tree/archive/feat/async-ui-and-update-verification) | [`49bd1c8`](https://github.com/Confetti3/SF4-Ember-Netplay/commit/49bd1c82cd77e611331be13ca81d969481de2afc) |
| `feat/rollback-diagnostics-and-pacing` | [Browse snapshot](https://github.com/Confetti3/SF4-Ember-Netplay/tree/archive/feat/rollback-diagnostics-and-pacing) | [`0626b69`](https://github.com/Confetti3/SF4-Ember-Netplay/commit/0626b6969387f56eeac211eb3a3b3584cec5b84a) |
| `fix/reliability-and-ux-issues` | [Browse snapshot](https://github.com/Confetti3/SF4-Ember-Netplay/tree/archive/fix/reliability-and-ux-issues) | [`f51717f`](https://github.com/Confetti3/SF4-Ember-Netplay/commit/f51717fa638d2eab2f7791004a75703e2bd3413d) |
| `security/audit-v0.6.5-launcher-vps` | [Browse snapshot](https://github.com/Confetti3/SF4-Ember-Netplay/tree/archive/security/audit-v0.6.5-launcher-vps) | [`f38a622`](https://github.com/Confetti3/SF4-Ember-Netplay/commit/f38a6220386640665209b7b4411b74108db70a37) |
| `test/steam-p2p-qt` | [Browse snapshot](https://github.com/Confetti3/SF4-Ember-Netplay/tree/archive/test/steam-p2p-qt) | [`c1b87f0`](https://github.com/Confetti3/SF4-Ember-Netplay/commit/c1b87f0569983697f8d7fed6b438cba87d51e56c) |

To resume work from an archived snapshot, create a new branch from its tag:

```sh
git fetch origin --tags
git switch -c restored-work refs/tags/archive/feat/async-ui-and-update-verification
```

[Legacy main](https://github.com/Confetti3/SF4-Ember-Netplay/tree/main) remains at `c6f2f1907f396c40a8542cc056e3bafa2adb3bde`. Existing release tags, releases and local feature checkouts are preserved.

## Historical documentation

The other documents in this directory describe retired Qt, direct-IP, broker and VPS releases. They are retained as history and are not current player or operator instructions. The active product uses Iroh private invitations. No remote infrastructure changes accompany this retirement.

[Read the pre-Ember README](README-pre-ember.md) · [Return to Ember](../../README.md)

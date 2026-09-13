# Branch archive

These snapshots preserve the 12 retired branches at their exact commits. They are stored as `archive/` tags so the branch selector stays focused on `release` and legacy `main`.

Select **Browse snapshot** to view the complete source and history for a retired branch.

| Former branch | Archived snapshot | Preserved commit |
| --- | --- | --- |
| `codex/v0.6.2-release` | [Browse snapshot](https://github.com/Confetti3/SF4-Ember-Netplay/tree/archive/codex/v0.6.2-release) | [`3a8cf22`](https://github.com/Confetti3/SF4-Ember-Netplay/commit/3a8cf22698904f98206872b9573f85a8e55e17d1) |
| `codex/v0.6.3-direct-ip-launcher` | [Browse snapshot](https://github.com/Confetti3/SF4-Ember-Netplay/tree/archive/codex/v0.6.3-direct-ip-launcher) | [`1ad6259`](https://github.com/Confetti3/SF4-Ember-Netplay/commit/1ad6259e219ab580d330be622aae5af8ecffb81d) |
| `dependabot/github_actions/actions/checkout-7` | [Browse snapshot](https://github.com/Confetti3/SF4-Ember-Netplay/tree/archive/dependabot/github_actions/actions/checkout-7) | [`f02c929`](https://github.com/Confetti3/SF4-Ember-Netplay/commit/f02c929d0623f41084a0aa3129265da1dc49f80f) |
| `dependabot/github_actions/actions/upload-artifact-7` | [Browse snapshot](https://github.com/Confetti3/SF4-Ember-Netplay/tree/archive/dependabot/github_actions/actions/upload-artifact-7) | [`e56b3de`](https://github.com/Confetti3/SF4-Ember-Netplay/commit/e56b3de7fa26fd63ea40bcfb158a32187b361b3b) |
| `dependabot/github_actions/azure/artifact-signing-action-2` | [Browse snapshot](https://github.com/Confetti3/SF4-Ember-Netplay/tree/archive/dependabot/github_actions/azure/artifact-signing-action-2) | [`6f1fdc3`](https://github.com/Confetti3/SF4-Ember-Netplay/commit/6f1fdc3e6686fd3315b6202eeef95e89f586eb47) |
| `dependabot/github_actions/azure/login-3` | [Browse snapshot](https://github.com/Confetti3/SF4-Ember-Netplay/tree/archive/dependabot/github_actions/azure/login-3) | [`4fe9ae1`](https://github.com/Confetti3/SF4-Ember-Netplay/commit/4fe9ae197c18f6fa4ae8a3a0591a0b267e62d1cf) |
| `docs/v0.6.6-rc1-notes` | [Browse snapshot](https://github.com/Confetti3/SF4-Ember-Netplay/tree/archive/docs/v0.6.6-rc1-notes) | [`53750e6`](https://github.com/Confetti3/SF4-Ember-Netplay/commit/53750e69069aa1a45dc6da09ad8b7fa8000497cb) |
| `feat/async-ui-and-update-verification` | [Browse snapshot](https://github.com/Confetti3/SF4-Ember-Netplay/tree/archive/feat/async-ui-and-update-verification) | [`a87a1ce`](https://github.com/Confetti3/SF4-Ember-Netplay/commit/a87a1ced112435117ec214aea4128a17f8f89b19) |
| `feat/rollback-diagnostics-and-pacing` | [Browse snapshot](https://github.com/Confetti3/SF4-Ember-Netplay/tree/archive/feat/rollback-diagnostics-and-pacing) | [`1324c26`](https://github.com/Confetti3/SF4-Ember-Netplay/commit/1324c26bf3d0d0fa7db0fb22d66344b73ac3f106) |
| `fix/reliability-and-ux-issues` | [Browse snapshot](https://github.com/Confetti3/SF4-Ember-Netplay/tree/archive/fix/reliability-and-ux-issues) | [`cf955ea`](https://github.com/Confetti3/SF4-Ember-Netplay/commit/cf955ea542aab46ae6568cb029c0ee9ff29a940a) |
| `security/audit-v0.6.5-launcher-vps` | [Browse snapshot](https://github.com/Confetti3/SF4-Ember-Netplay/tree/archive/security/audit-v0.6.5-launcher-vps) | [`bfd4457`](https://github.com/Confetti3/SF4-Ember-Netplay/commit/bfd44574711bc403b13fa95323dbeda2a7ae47c9) |
| `test/steam-p2p-qt` | [Browse snapshot](https://github.com/Confetti3/SF4-Ember-Netplay/tree/archive/test/steam-p2p-qt) | [`4c9a765`](https://github.com/Confetti3/SF4-Ember-Netplay/commit/4c9a7654f24a4fe7899fb64e3dc3d655e447dc57) |

To resume work from an archived snapshot, create a new branch from its tag:

```sh
git fetch origin --tags
git switch -c restored-work refs/tags/archive/feat/async-ui-and-update-verification
```

[Legacy main](https://github.com/Confetti3/SF4-Ember-Netplay/tree/main) remains at `53750e69069aa1a45dc6da09ad8b7fa8000497cb`. Existing release tags, releases and local feature checkouts are preserved.

## Historical documentation

The other documents in this directory describe retired Qt, direct-IP, broker and VPS releases. They are retained as history and are not current player or operator instructions. The active product uses Iroh private invitations. No remote infrastructure changes accompany this retirement.

[Read the pre-Ember README](README-pre-ember.md) · [Return to Ember](../../README.md)

# Windows Defender and antivirus detections

Game-hook software can trigger heuristic detections, but a detection name alone
does not establish that a particular file is safe or a false positive. This
development build is unsigned. Signing identifies a publisher; it does not
guarantee that a file is harmless or that antivirus will accept it.

If a file is blocked:

1. Keep protection enabled. Do not add broad folder exclusions or disable scanning.
2. Confirm where the build came from. For an official release, use the project's
   [release page](https://github.com/Confetti3/SF4-Ember-Netplay/releases).
3. Compare the ZIP's SHA-256 against the expected hash. A complete release's
   `preflight.cmd` checks its extracted inventory. These checks detect changes
   relative to their records; they are not independent publisher authentication.
4. Record the exact detection, affected filename and build. Contact the maintainer
   privately if further investigation is needed. A suspected false positive can
   be submitted to [Microsoft for analysis](https://www.microsoft.com/en-us/wdsi/filesubmission).
5. Wait for a reviewed resolution instead of treating a detection as automatically
   safe to allow. Keep the previous working build available.

The project does not ship antivirus-exclusion scripts. Current signing status
and the unfinished release-authentication work are documented in
[Release authentication](../development/CODE_SIGNING.md).

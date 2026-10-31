Reviewed only `bdb5bd50~1..bdb5bd50` against the supplied design. **Changes are required**, principally in how symbols become durable, resumable publication artifacts.

1. **[P2] Symbol upload is hidden inside packaging and breaks publication recovery.**  
   **Files/functions:** [github-release.ps1: `Package`](C:/Users/Kate/Desktop/sf4/sf4-nightly/scripts/github-release.ps1:19), [publish-nightly.ps1: `Package`](C:/Users/Kate/Desktop/sf4/sf4-nightly/scripts/publish-nightly.ps1:29).

   Both callbacks finish `New-EmberPackage` before validating and uploading symbols. If symbol validation, SSH, or SCP fails, the package directory already exists, but the publication’s pending record has not been saved. Retrying Stable publication calls `New-EmberPackage` again and fails with “Package destination already exists.” This adds a required network side effect to an operation that the publication state machine treats as local preparation.

   **Remedy:** validate symbols before packaging, then make symbol upload an explicit, shared, resumable publication phase. Save the immutable artifacts before uploading, and record completion so retries reuse the package and symbols without rebuilding or repackaging. Add a publication fixture where symbol upload fails once and the next invocation completes.

2. **[P2] Installer commit and pruning are not atomic across publishers.**  
   **File/functions:** [install-symbols.sh: `commit` / `prune_nightly`](C:/Users/Kate/Desktop/sf4/sf4-nightly/server/ember-reports/deploy/install-symbols.sh:59).

   Directory renames prevent partially copied files from becoming visible, but there is no lock around the relationship between installed directories and release manifests. For example, Stable can reuse an existing dependency DEBUGID while Nightly pruning snapshots the manifests before Stable writes its manifest. Pruning then deletes that dependency’s directory; Stable subsequently records it and reports success. The resulting Stable manifest references missing symbols despite the promise that Stable symbols remain.

   **Remedy:** take one store-wide `flock` for the entire commit operation, including existing-directory checks, directory installation, manifest replacement, and pruning. Uploading into the incoming directory can remain outside the lock. Add an interleaving test covering a dependency shared with an expired Nightly manifest.

3. **[P2] The required local symbol archive is missing.**  
   **Files/functions:** [BuildProvenance.ps1: `New-EmberSymbols`](C:/Users/Kate/Desktop/sf4/sf4-nightly/scripts/BuildProvenance.ps1:113), [upload-symbols.ps1: upload flow](C:/Users/Kate/Desktop/sf4/sf4-nightly/scripts/upload-symbols.ps1:20).

   Symbols exist only in the mutable `<build>/symbols` directory, which the next symbols build deletes. Neither publish path preserves a copy per release label. The packaged receipt records an absolute store path and module IDs, so retaining the package does not retain the symbols. After another build, the documented manual recovery command cannot restore the earlier release’s store from local publication artifacts.

   **Remedy:** archive the verified symbol store and its receipt under the release label beside the local package, before upload. Record that archive in the publication state and use it for upload and recovery. This also gives finding 1 a durable artifact to resume from.

4. **[P2] Timeout classification silently narrows the agreed retry policy.**  
   **File/function:** [ReportWorkflow.cxx: `DumpInTheWay`](C:/Users/Kate/Desktop/sf4/sf4-nightly/src/platform/ReportWorkflow.cxx:130).

   A client timeout triggers fallback only when `statusCode == 0`. The transport assigns `statusCode` before reading the response body, and a later read can fail with `HttpErrorKind::Timeout` while retaining that status. Such a timeout bypasses fallback unless the status happens to be 408 or 413. The new tests cover only the zero-status timeout shape.

   **Remedy:** classify transport timeout explicitly rather than assuming it implies no HTTP status, while preserving cancellation and the required 429 exclusion. Add a timeout case with headers already received, and define how an incomplete 202 response should be treated.

5. **[P2] The symbol generator is recorded, but never pinned.**  
   **File/function:** [BuildProvenance.ps1: `Get-EmberDumpSyms`](C:/Users/Kate/Desktop/sf4/sf4-nightly/scripts/BuildProvenance.ps1:94).

   Preflight checks only that `~/.cargo/bin/dump_syms` is executable. The installation instruction uses `cargo install dump_syms --locked` without a version, and `--version` is recorded only after generation. Consequently, publishers can use different generator releases despite the design’s explicit version pin.

   **Remedy:** declare the supported generator version, check it during preflight, and include `--version` in the installation command. Add a fixture proving that an unsupported version fails before generation.

The remaining inspected areas are sound at the source level: the consent rename uses the canonical preference table without an old-key read alias; the automatic fallback retains one reservation and permits at most two uploads; generation compares all five source DLL hashes with the staged receipt; and the server constants and service memory allocation agree with the chosen two-worker default. No changed source or script crosses 1,000 lines. All 14 catalogs update the same five messages, preserve placeholders, and introduce no new catalog characters.

Validation could not execute under this session’s read-only permissions. CTest failed creating its log; the four publisher script suites failed creating temporary fixtures. WSL returned access denied, so paths containing spaces remain unverified live. These are environment blocks, not passing or failing implementation tests. The working-tree status remained unchanged; no build, deployment, or VPS access occurred.

VERDICT: CHANGES REQUIRED

# Controller and selection implementation scope

Authorization: the user explicitly approved the controller/HUD/gallery implementation plan and clarified that all SF4-supported controller types must work.

In scope: local source changes in sf4-ember-rooms, read-only inspection of a copied installed SSFIV executable, builds, synthetic/native-adapter tests, and a local candidate package.

Network profile: local test helpers only. No external publication, player messaging, game installation or game launch. The separate training worktree is outside the edit scope.

## Follow-up: shorter invite text

The user clarified that invitations should contain less text, rather than use smaller UI controls. This authorizes a compact invitation encoding and its parser/tests, while retaining room admission credentials, routing, expiry and build checks. The refreshed local package includes this change. Installation, game launch and publication remain separate steps.

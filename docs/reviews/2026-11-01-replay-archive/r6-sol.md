**R6-01 — P1: Prefix mode forwards the listing to a nonexistent bridge route.**

Location: [server/ember/archive-site/deploy/nginx/archive-locations.conf.in:34](/C:/Users/Kate/Desktop/sf4/sf4-nightly/server/ember/archive-site/deploy/nginx/archive-locations.conf.in:34).

With the default `PREFIX=/archive`, the listing location matches `/archive/v1/archive`, but `proxy_pass http://@ZONE@_bridge;` has no URI component. Nginx therefore forwards `/archive/v1/archive?...` unchanged. The bridge registers only `/v1/archive` at [routes/mod.rs:99](/C:/Users/Kate/Desktop/sf4/sf4-nightly/server/ember/bridge/src/routes/mod.rs:99), so the real bridge returns 404 and the archive page cannot load its listing. This follows nginx’s documented [`proxy_pass` URI rules](https://nginx.org/en/docs/http/ngx_http_proxy_module.html#proxy_pass).

The installation check also lets this failure through: [setup.sh:409](/C:/Users/Kate/Desktop/sf4/sf4-nightly/server/ember/archive-site/deploy/setup.sh:409) treats a non-200 listing as an advisory message, then disarms rollback and finishes successfully.

**Fix:** use the canonical upstream path directly in the shared template:

```nginx
proxy_pass http://@ZONE@_bridge/v1/archive;
```

This preserves query parameters and works in both prefix and own-site modes, with no additional branching. Make the stand-in accept only `/v1/archive` and assert that a prefixed request reaches that path with its query intact. Treat an upstream 404 as a deployment failure while retaining any intentionally supported bootstrap response.

I found no additional P1 or P2 findings in the requested scope. The alias locations have matching trailing slashes, avoiding the alias-prefix traversal mismatch; nginx normalizes dot segments before location matching. The redirect, snippet naming and glob, missing-include refusal, prefix-mode rollback tracking and install marker, and staging/production zone separation are otherwise consistent. The shared extraction keeps both deployment modes straightforward.

The relative HTML, JavaScript and CSS URLs resolve correctly under `/` and `/archive/`. The tournament-flow changes correctly send replays after both reports and retain distinct, bridge-clock-based replay bytes for the pack assertion.

Validation was read-only: source and scoped-diff review, in-memory URL/asset checks, and a passing scoped `git diff --check`. I edited nothing, built nothing, and did not rerun the supplied runtime tests. The finding above is source-derived; the reported successful listing check warrants comparing its rendered configuration with this template.

VERDICT: CHANGES REQUIRED
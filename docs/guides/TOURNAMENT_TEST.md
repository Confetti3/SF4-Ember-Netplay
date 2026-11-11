# Testing tournament mode

The web desk's own logic (sets, tiers, counters) has unit tests in Node 22:

```sh
node --test server/ember/bridge/desk/test/sets.test.js
```

Two games playing a match of a local test bridge end to end, with nobody at
either game, is the tournament self-test (docs/design/TOURNAMENT_SELFTEST.md).

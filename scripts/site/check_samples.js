// The pages' own readers (assets/answers.js) over the preview's made-up answers, so a sample a page
// would refuse to draw fails the site checks: node check_samples.js ASSETS SAMPLES.json
// SAMPLES holds, for each session state, the answers of /account/me.json (me), of every board
// (boards, by fighter, "" for the account's) and of /account/card.json (cards). test_site.py writes it
// from preview_accounts.py and runs this.
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const [assets, samplesPath] = process.argv.slice(2);
const samples = JSON.parse(fs.readFileSync(samplesPath, 'utf8'));
// The readers need no page: site.js's helpers are only called when drawing.
const context = { console, Intl, URLSearchParams, location: { origin: 'http://localhost' } };
context.window = context;
vm.createContext(context);
for (const name of ['site', 'marks', 'answers']) {
  vm.runInContext(fs.readFileSync(path.join(assets, name + '.js'), 'utf8'), context, { filename: name + '.js' });
}
const answers = context.EmberAnswers;
const refused = [];
let read = 0;
function check(what, model) {
  read += 1;
  if (!model) refused.push(what);
  return model;
}

for (const [state, sample] of Object.entries(samples)) {
  const me = check(`${state}: /account/me.json`, answers.session(sample.me));
  if (me && me.state === 'signed_in') {
    check(`${state}: the rating details of /account/me.json`, me.details);
    if (me.details && (me.details.rating === null) !== (sample.me.rating === undefined)) refused.push(`${state}: the rating read as ${me.details.rating}`);
  }
  for (const [fighter, body] of Object.entries(sample.boards)) {
    check(`${state}: /points.json?fighter=${fighter}`, answers.board(body, fighter === '' ? null : Number(fighter)));
  }
  for (const [asked, body] of Object.entries(sample.cards)) check(`${state}: /account/card.json ${asked}`, answers.lookup(body));
}

if (refused.length) {
  console.error('The readers refused these preview samples:\n  ' + refused.join('\n  '));
  process.exit(1);
}
console.log(`${read} preview answers read`);

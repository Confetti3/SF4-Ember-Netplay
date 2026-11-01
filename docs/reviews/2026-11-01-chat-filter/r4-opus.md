Review by Opus (Sol had reached its usage limit), against r4-prompt.md.

P1, ordinary chat still hidden:
1. stems.txt "shit", "nigger", "nigga" match inside ordinary words: Japanese romaji ("arigatou gozaimashita", "otsukaresama deshita", "doushite", "shitsurei shimashita") and English ("sniggering", "niggardly"). allow.txt cannot take a stem back. Fix: drop the bare "shit" stem for stems ending in a consonant (shitt, shith, shitf, shitl, bullshit, dipshit, horseshit); rely on the list entries for the two slurs; add the romaji to the corpus.
2. allow.txt collisions: Dutch "lul" (Twitch's LUL); Czech/Polish "debil" and 白痴/白癡 (mild, against the header rule); Dutch "godverdomme" and "gadverdamme"; 老味 (inside 老味道); 撒尿 (pee, allowed elsewhere); for consistency 宦官 and 情色.

P2: the per-list unspaced flag repeats the per-entry check and List::tag is never read. Classify each entry by its own characters and generate a plain list of lists.

P3: duplicate target_include_directories in CMakeLists.txt (the module should own it); squeezed comparison loses the entry's doubled letters ("connnn" matches "coon", "Bobbb" matches "boob"), compare letter runs instead; shell_room_journey_test's in-room Room layout row list lacks chat-filter.

Checked: UI ownership, once-per-message filtering, raw-text echo matching, folding, cost, reconfigure, notices, copy in 14 locales, file sizes.

VERDICT: CHANGES REQUIRED

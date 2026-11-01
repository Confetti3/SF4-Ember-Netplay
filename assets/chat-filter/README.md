# Chat filter word lists

The room chat's filter (Settings › Interface › Room layout › Chat filter, on by default) hides the words on these lists on the player's own screen. `src/ui/ChatFilter.cxx` reads them; `cmake/sf4e_chat_filter.cmake` embeds them in the build.

## Source

The language lists are the [List of Dirty, Naughty, Obscene, and Otherwise Bad Words](https://github.com/LDNOOBW/List-of-Dirty-Naughty-Obscene-and-Otherwise-Bad-Words) by Shutterstock and its contributors, licensed [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/). The distributed licence is `LICENSE`; packages include it in `notices/THIRD_PARTY_LICENSES.txt`.

They were downloaded unmodified from commit `5faf2ba42d7b1c0977169ec3611df25a3c08eb13` (13 July 2020, the newest on `master` on 1 November 2026), one file per language Ember is translated into, each saved as `<language>.txt`:

| File | SHA-256 |
| --- | --- |
| `cs.txt` | `90706df86709bade1ca39cf9f9d9836409c674fc3bffcb7525bdd20e59b596bc` |
| `de.txt` | `ebe44d7772897390e27f7bec412fdcdf0efc294e4db157f8487c4cd8d4fabd55` |
| `en.txt` | `af851ecef1d5f212caba17339b12ac39cc2fef7d78c74876f67237644fcee8bd` |
| `es.txt` | `073334261cc2e7c08339faefd2f19f9623e8240a0ac18fe2a26c368b897496ce` |
| `fr.txt` | `79763a705e08256b393952852bb63aa903f4485e0ef02f70735f5e50efff213c` |
| `it.txt` | `d413f41018d58aa79d2c4809d0161f29ffd5652c4dc34c5b7b9ade680ee5599a` |
| `ja.txt` | `f83c4d0843ea6474c17bf171edd72b616b3af403f8613f2cde1bd841faf6f316` |
| `ko.txt` | `0dd4b74ffea6e3bb35d658e6d8262b53ee99281eaa48ecb515a2065bfcc7f69d` |
| `nl.txt` | `8298ba0cdc3ca9a1989c349b8ad0148e4504c2804f2f2fb6c2f52bfb39c5ae71` |
| `pl.txt` | `98cf824e3692255645c5a72d5913f26fd4f20930f725683b4c96bb312ca5d9b3` |
| `pt.txt` | `05ef524b0bb7f83f87257f6ec4dabf70e16b8bb6930f769fb93deaf608ff196d` |
| `ru.txt` | `2598adabd2b761733d72a68d8ee4fcc1d2c38f2f85b6ff489985c6ce1d93bf75` |
| `zh.txt` | `a1d9aa037c8b039ef3b40148b3364ce2ca62ce4a955b7082a16ad99f6cbd1bc0` |
| `LICENSE` | `55bfbc1759943f7d19acfc68e35644b7fbe10cd8d39ab3551ce5ecff5dda9979` |

## Ember's changes

The files above are kept as downloaded. Two files of Ember's own change what the filter does with them:

- `allow.txt`: entries the filter leaves alone. The lists were made to keep search suggestions clean, so they hold everyday words a game chat needs (English "sucks", Spanish "con", Portuguese "comer", Dutch "nicht", Japanese 女の子, Chinese 成人), words for who someone is, and mild words the English list leaves out.
- `stems.txt`: a few English stems hidden inside any word, for the forms the lists leave out (fucked, bullshitter).

The filter also skips entries it cannot match safely: a single Japanese or Chinese character (性 is inside hundreds of ordinary words), and entries made only of single letters (s&m).

## Updating

Download the same files from a newer commit, update the table, and check the new entries against everyday chat: anything ordinary goes in `allow.txt`.

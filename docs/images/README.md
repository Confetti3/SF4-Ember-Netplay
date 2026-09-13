# Ember 0.8.0 interface captures

These PNGs are captured directly from the production Ember UI using `UiRenderTest`
with the `--readme-shots` option. The room names, members, chat and profile are fictional sample data. The game is not launched by this renderer.
Portraits are decoded from an owned Steam installation; selection artwork comes
from the repository's credited assets. See [attribution](../../ATTRIBUTION.md).

Generate after building the current source:

```powershell
pwsh -NoProfile -File ./scripts/capture-readme.ps1 -GameRoot 'C:/Games/Steam/steamapps/common/Super Street Fighter IV - Arcade Edition'
```

Home, room, fighter and costume selection retain the full 1280 x 720 viewport.
Only BMP-to-PNG conversion is applied; UI text, geometry and artwork are not
composited or retouched.

The normal UI regression suite retains its long-name and long-chat stress cases;
the README option substitutes readable sample names and chat for presentation.

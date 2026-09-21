# Saved Fighter Select test

1. Extract the complete package and run Launcher. At the native main menu, open Ember with F10 if needed.
2. Open **Fighter Select** before joining a room. Choose an available fighter, appearance, Ultra and stage.
3. Switch to Play and back: the selection remains. Restart the launcher and verify it persists.
4. Join a room and take a seat while alone. Confirm that you can still edit the saved fighter.
5. When an opponent joins, use **Ready** in Play. The chosen fighter, costume, color and Ultra must appear in the actual match. P1's stage applies.
6. After the match, **Ready for rematch** reuses the choice. Use **Unready**, then **Change fighter**, to change it before readiness.
7. With Edition Select disabled or an unavailable appearance, open Fighter Select and choose a supported option. Ready must not send an unavailable pick.

The former isolated preview has been removed. The gallery now edits the persisted online match selection. Native availability applies to costumes and colors; missing artwork is labelled without making an unavailable choice playable.

Automated rendering checks cover page navigation, pre-room selection, appearance and Ultra changes, stage permissions, and the saved values observed at Ready/rematch. Actual two-PC gameplay and hardware input require the separate controller/HUD acceptance run.

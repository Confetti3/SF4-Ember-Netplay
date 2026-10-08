#pragma once
// Custom costumes, installed by a separate setup program (it downloads them and adds them to the game; Ember
// never downloads anything). A custom costume is slot 8..99 of a fighter (costume index 7..98) with its files in
// the game's patch folder: patch_ae2_tu3\battle\chara\<CHR>\<CHR>_<NN>.obj.emo and the rest. Nothing extra goes
// over the network: the costume number already travels in each pick. Each PC shows the costume it has in that
// slot, and a PC without one shows the fighter's original costume instead.
//
// The game itself never sees a custom costume number: its per-costume tables hold only the fighter's own
// costumes (7 or 8; Dimps::Game::Battle::Chara::CharaResourceHolder, RVA 0x1e30b0 reads them unchecked). A
// custom costume plays as a stand-in, one of the fighter's first three costumes (always owned), and while the
// game builds that stand-in's file names it is given the custom slot instead (ApplyPicks, Install).
#include "../common/FighterCatalog.hxx"
#include "../common/StageCatalog.hxx"

#include <vector>

namespace sf4e { namespace custom {

// The file-name hooks (Steam 1.05): the CostumeResourceHolder's load (RVA 0x1e5f80: model, shadow, normals,
// physics, sound bank), the ColorResourceHolder's (RVA 0x1e5cf0: colour textures, materials), and the stage
// code lookup (RVA 0x2862c0) as the stage loader uses it.
// Call inside the sidecar's Detours transaction.
void Install();

// Custom costume indices installed on this PC for a fighter, ascending. Read once per game start (the setup
// program only changes them while the game is closed). SF4E_IGNORE_CUSTOM=1 in the environment pretends none
// are installed, to play a player without them (testing with a second copy).
const std::vector<int>& InstalledCostumes(int fighterId);
bool CostumeInstalled(int fighterId, int costumeId);
// Custom stage ids installed on this PC (battle\stage\STG_<code>.emz in the patch folder with a custom code, see
// StageCatalog), ascending.
const std::vector<int>& InstalledStages();

// The stage this PC hands the game for a match's stage id: a stock stage stays; a custom stage plays as its
// fallback stage (its effects), and if this PC has it, the stage loader is given its code (C12: STG_C12.emz and
// the scripts in it) instead of the fallback's until EndBattle, and so is the music if the stage has its own
// (battle\sound\bgm\BGM_C12.csb in the patch folder); otherwise the fallback's music plays.
int ApplyStage(int stageId);

// Battle side `side` plays custom costume `custom` of `fighter` as costume `standIn` until EndBattle.
void SetStandIn(int side, int fighter, int standIn, int custom);
void EndBattle();

// What this PC hands the game for both picks of a match (fighters and spectators alike):
// - a custom costume it doesn't have becomes the fighter's original costume in colour 1, or colour 2 if that would
//   look identical to the other side's pick (same fighter, costume and colour);
// - a custom costume it has plays as a stand-in costume the other side isn't using.
template<class Native> void ApplyPicks(Native (&picks)[2]) {
    bool fellBack[2] = {};
    for (int i = 0; i < 2; i++)
        if (selection::IsCustomCostume(picks[i].costume) && !CostumeInstalled(picks[i].charaID, picks[i].costume)) {
            picks[i].costume = 0;
            picks[i].color = 0;
            fellBack[i] = true;
        }
    for (int i = 0; i < 2; i++) {
        const Native& other = picks[1 - i];
        if (fellBack[i] && other.charaID == picks[i].charaID && other.costume == picks[i].costume &&
            other.color == picks[i].color)
            picks[i].color = picks[i].color == 0 ? 1 : 0;
    }
    for (int i = 0; i < 2; i++) {
        SetStandIn(i, -1, -1, -1);
        if (!selection::IsCustomCostume(picks[i].costume)) continue;
        const Native& other = picks[1 - i];
        for (int standIn : {1, 2, 0})
            if (other.charaID != picks[i].charaID || other.costume != standIn) {
                SetStandIn(i, picks[i].charaID, standIn, picks[i].costume);
                picks[i].costume = static_cast<decltype(picks[i].costume)>(standIn);
                break;
            }
    }
}

} }

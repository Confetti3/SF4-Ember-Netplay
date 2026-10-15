#pragma once
// Custom costumes, colors and stages, installed by a separate setup program (it downloads them and adds them to the
// game; Ember never downloads anything). A custom costume is slot 8..99 of a fighter (costume index 7..98) with its
// files in the game's patch folder: patch_ae2_tu3\battle\chara\<CHR>\<CHR>_<NN>.obj.emo and the rest. Nothing extra
// goes over the network: the costume, color and stage numbers already travel in each pick. Each PC shows what it
// has, and a PC without it the fighter's original costume, color 1, or a stock stage (selection::ResolveAppearance,
// selection::CustomStageFallback).
//
// The game itself never sees a custom costume number: its per-costume tables hold only the fighter's own
// costumes (7 or 8; Dimps::Game::Battle::Chara::CharaResourceHolder, RVA 0x1e30b0 reads them unchecked). A
// custom costume plays as a stand-in, one of the fighter's first three costumes (always owned), and while the
// game builds that stand-in's file names it is given the custom slot instead (UseAppearance, Install).
#include "../common/InstalledContent.hxx"

namespace sf4e { namespace custom {

// The file-name hooks (Steam 1.05): the CostumeResourceHolder's load (RVA 0x1e5f80: model, shadow, normals,
// physics, sound bank), the ColorResourceHolder's (RVA 0x1e5cf0: color textures, materials), and the stage
// code lookup (RVA 0x2862c0) as the stage loader uses it.
// Call inside the sidecar's Detours transaction.
void Install();

// What is installed on this PC, scanned once (the setup program only changes it while the game is closed) and
// never changed after: the same object for the whole run. SF4E_IGNORE_CUSTOM=1 in the environment pretends nothing
// is installed, to play a player without it (testing with a second copy).
selection::InstalledContentPtr Installed();

// The stage this PC hands the game for a match's stage id: a stock stage stays; a custom stage plays as its
// fallback stage (its effects), and if this PC has it, the stage loader is given its code (C12: STG_C12.emz and
// the scripts in it) instead of the fallback's until EndBattle, and so is the music if the stage has its own
// (battle\sound\bgm\BGM_C12.csb in the patch folder); otherwise the fallback's music plays.
int ApplyStage(int stageId);

// Installs a match's swaps (selection::ResolveAppearance) until EndBattle: the hooks give the game each side's
// custom costume and color while it loads their stand-ins.
void UseAppearance(const selection::Appearance& appearance);
void EndBattle();

} }

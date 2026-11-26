#include "sf4e__BattleFighters.hxx"

#include "../Dimps/Dimps__Game__Battle__Chara.hxx"
#include "../Dimps/Dimps__Game__Battle__System.hxx"

sf4e::training::FighterRoots sf4e::battle::ReadRoots(Dimps::Game::Battle::System* system) {
	using System = Dimps::Game::Battle::System;
	using Actor = Dimps::Game::Battle::Chara::Actor;
	using Unit = Dimps::Game::Battle::Chara::Unit;
	training::FighterRoots roots{};
	Unit* unit = system ? (system->*System::publicMethods.GetCharaUnit)() : nullptr;
	for (unsigned side = 0; side < 2 && unit; ++side)
		if (Actor* actor = (unit->*Unit::publicMethods.GetActorByIndex)(side)) roots[side] = (actor->*Actor::publicMethods.GetCurrentRootPosition)();
	return roots;
}

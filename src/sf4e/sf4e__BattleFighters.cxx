#include "sf4e__BattleFighters.hxx"

#include "../Dimps/Dimps__Game__Battle__Chara.hxx"
#include "../Dimps/Dimps__Game__Battle__System.hxx"

using Actor = Dimps::Game::Battle::Chara::Actor;

std::array<Actor*, 2> sf4e::battle::ReadActors(Dimps::Game::Battle::System* system) {
	using System = Dimps::Game::Battle::System;
	using Unit = Dimps::Game::Battle::Chara::Unit;
	std::array<Actor*, 2> actors{};
	Unit* unit = system ? (system->*System::publicMethods.GetCharaUnit)() : nullptr;
	for (unsigned side = 0; side < 2 && unit; ++side) actors[side] = (unit->*Unit::publicMethods.GetActorByIndex)(side);
	return actors;
}

sf4e::training::FighterRoots sf4e::battle::ReadRoots(Dimps::Game::Battle::System* system) {
	training::FighterRoots roots{};
	const auto actors = ReadActors(system);
	for (unsigned side = 0; side < 2; ++side) if (actors[side]) roots[side] = (actors[side]->*Actor::publicMethods.GetCurrentRootPosition)();
	return roots;
}

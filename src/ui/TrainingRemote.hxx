#pragma once
#include "GameMenu.hxx"
#include "TrainingPanel.hxx"
#include "../training/PracticeSettings.hxx"
#include "../training/ComboCheck.hxx"
#include <filesystem>
#include <string>
#include <vector>

// The Phone remote page of the training controls (TrainingPanel.cxx draws the
// lab's list; TrainingRemote.cxx keeps the remote's own rows, its QR card and
// what Select does on them) and the phone's changes to the lab.
namespace sf4e { namespace ui {
// Whether the overlay offers the remote (SetTrainingRemote); the UI lab does not.
bool TrainingRemoteOffered();
std::vector<MenuEntry> TrainingRemoteRows();
// The QR code over the list while the page asks for it; none off the page.
GameMenu::ListCard TrainingRemoteCard(bool onPage);
// What Select, Left or Right did on the page; `error` is the status line's.
void HandleTrainingRemote(const MenuAction& action, std::string& error);

// The lab's side, kept in TrainingPanel.cxx: its settings the phone changes in
// place (read from training.json first), on the overlay thread only.
struct TrainingLabParts {
    training::MeterOptions& meter; training::DummyPlan& plan; training::combo_check::Settings& check;
    training::ReplySlots& slots; const std::filesystem::path& directory;
};
TrainingLabParts TrainingLab();
// Writes training.json; sends the dummy's plan with its reply (false, and said, when it cannot go).
void SaveTrainingLab();
bool SendTrainingPlan(const training::View& view, const TrainingSubmit& submit);
} }

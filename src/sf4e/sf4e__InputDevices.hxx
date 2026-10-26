#pragma once
#include "../netplay/InputAssignment.hxx"
namespace sf4e { namespace input {
// Call only on the game thread after native main-menu readiness.
// The keyboard row's buttons cost a 248-key scan. Its one reader is a capture
// waiting for every key to be released, so a caller with capture idle passes
// false and the row reads 0 (ScanKeyboard fills it in later if a capture
// starts in the same tick).
std::vector<Device> ReadDevices(bool scanKeyboard = true);
void ScanKeyboard(std::vector<Device>& devices);
void ReleaseFromSide(int side);
bool AssignToSide(const Device& device, int side, bool fight);
// Game-thread live input seam. Fails neutral on disconnected/replaced devices
// or a native side mapping that no longer matches the established owner.
bool ReadAssignedInput(const Device& device, int side, unsigned& mapped, unsigned& raw);
// Whether the game holds an assignment for side. The input update reads a
// side without one from every pad and keyboard (0x511E70, 0x510BE0), the
// other player's included, whatever device type and index it still names.
bool SideAssigned(int side);
Device MenuDevice(const std::vector<Device>& devices);
// Logs the game's pads, the HID controllers behind them and whether Steam's
// overlay (without which Steam Input cannot apply) is in the game, on change.
void LogInventory(const std::vector<Device>& devices);
} }

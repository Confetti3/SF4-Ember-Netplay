#pragma once
#include "../netplay/InputAssignment.hxx"
namespace sf4e { namespace input {
// Call only on the game thread after native main-menu readiness.
std::vector<Device> ReadDevices();
void ReleaseFromSide(int side);
bool AssignToSide(const Device& device, int side, bool fight);
// Game-thread live input seam. Fails neutral on disconnected/replaced devices
// or a native side mapping that no longer matches the established owner.
bool ReadAssignedInput(const Device& device, int side, unsigned& mapped, unsigned& raw);
Device MenuDevice(const std::vector<Device>& devices);
} }

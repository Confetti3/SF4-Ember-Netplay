#pragma once

// The summary printed at the end of a run, with each room's verdict.
#include "soak_workload.hxx"

namespace sf4e { namespace test { namespace soak {

// Prints every room's figures and failed criteria; sets failed when any room failed.
void PrintSummary(const std::vector<std::unique_ptr<Room>>& rooms, Clock ranMs, bool& failed);

} } }

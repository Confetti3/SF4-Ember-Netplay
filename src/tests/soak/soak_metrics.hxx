#pragma once

// The CSV: one row per room per minute (WriteRow), with the helpers' and the
// client's own memory and processor use.
#include "soak_workload.hxx"

namespace sf4e { namespace test { namespace soak {

// Every room thread writes its own rows; the process-wide figures come from the
// main thread and from what the other rooms last published.
extern std::mutex csvMutex;
extern std::atomic<double> processCpuPct;
extern std::vector<Room*> allRooms;
extern const char* const CsvHeader;

// Processor time of a process in milliseconds.
std::uint64_t ProcessTime(HANDLE process);
// Checks the room's chat and writes its row for the minute ending at now.
void WriteRow(std::ofstream& csv, Room& room, Clock now);

} } }

#pragma once
#include "PackageInstaller.hxx"

namespace sf4e { namespace launcher {
// Preparation may finish work after its last report. Poll once more with the
// same counters before handing the installation to another process.
template<class Spawn>
bool HandoffPreparedUpdate(const PackageProgress& progress, std::uint64_t done, std::uint64_t total, Spawn&& spawn) {
    if(progress && !progress(done,total)) return false;
    spawn();
    return true;
}
} }

#include "SessionRecovery.hxx"
#include "../common/CoordinationBytes.hxx"

namespace sf4e { namespace session { namespace recovery_detail {

// The same helper as checkpoint transfers: CNG with one provider handle for
// the process, the portable digest off Windows or when CNG fails.
std::string Sha256(const std::string& input) { return coordination::Sha256(input); }

} } }

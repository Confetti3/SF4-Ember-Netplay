// SF4's room host: its catalog is SF4's roster and stages, compiled in
// (src/product/Sf4Catalog.cxx), and its build is the Sidecar.dll hash the
// supervisor keys it by, which the host does not compute. It hosts only
// "usf4" rooms.
#include "RoomHost.hxx"

namespace sf4e { namespace roomhost {

bool PrepareCatalog(const Config& config, std::string& reason) {
	if (config.game == "usf4") return true;
	reason = "other_game";
	return false;
}

} }

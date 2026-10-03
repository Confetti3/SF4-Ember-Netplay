#pragma once
// The two plain structs the session protocol needs from the game-binding
// headers. On Windows the real Dimps headers (windows.h, d3d9.h) define them;
// anywhere else (the Linux room host) this header stands in with the same
// names, layout and JSON form. Keep the two in step.
#ifdef _WIN32
#include "../Dimps/Dimps__GameEvents.hxx"
#include "../Dimps/Dimps__Math.hxx"
#else
#include <nlohmann/json.hpp>
#include "../common/SelectionValue.hxx"

// SessionProtocol.hxx declares one DWORD field; the Windows headers supply the type.
typedef unsigned int DWORD;

namespace Dimps {
	namespace Math {
		struct FixedPoint {
			unsigned short fractional;
			short integral;
		};
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(FixedPoint, fractional, integral);
	}
	namespace GameEvents {
		struct VsMode {
			struct ConfirmedCharaConditions {
				unsigned char charaID;
				unsigned char costume;
				unsigned char color;
				unsigned char _unused;
				unsigned char personalAction;
				unsigned char winQuote;
				unsigned char ultraCombo;
				unsigned char handicap;
				unsigned char unc_edition;
			};
		};

		inline void to_json(nlohmann::json& j, const VsMode::ConfirmedCharaConditions& c) {
			j = {{"charaID", c.charaID}, {"costume", c.costume}, {"color", c.color},
				{"_unused", c._unused}, {"personalAction", c.personalAction}, {"winQuote", c.winQuote},
				{"ultraCombo", c.ultraCombo}, {"handicap", c.handicap}, {"unc_edition", c.unc_edition}};
		}

		inline void from_json(const nlohmann::json& j, VsMode::ConfirmedCharaConditions& c) {
			using sf4e::selection::ReadByte;
			VsMode::ConfirmedCharaConditions decoded{};
			decoded.charaID = ReadByte(j.at("charaID"));
			decoded.costume = ReadByte(j.at("costume"));
			decoded.color = ReadByte(j.at("color"));
			decoded._unused = ReadByte(j.at("_unused"));
			decoded.personalAction = ReadByte(j.at("personalAction"));
			decoded.winQuote = ReadByte(j.at("winQuote"));
			decoded.ultraCombo = ReadByte(j.at("ultraCombo"));
			decoded.handicap = ReadByte(j.at("handicap"));
			decoded.unc_edition = ReadByte(j.at("unc_edition"));
			c = decoded;
		}
	}
}
#endif

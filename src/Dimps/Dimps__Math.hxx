#pragma once

#include <windows.h>
#include <nlohmann/json.hpp>
#include "../common/MatchWireTypes.hxx"

namespace Dimps {
	namespace Math {
		// The engine's fixed-point value is the session wire's (MatchWireTypes.hxx).
		using FixedPoint = sf4e::wire::FixedPoint;

		struct Vec4F {
			float x;
			float y;
			float z;
			float w;
		};

		struct Matrix4x4 {
			float mat[16];
		};

		float FixedToFloat(FixedPoint* fp);

	}
}
#include "Dimps__Math.hxx"

using Dimps::Math::FixedPoint;

float Dimps::Math::FixedToFloat(FixedPoint* fp) {
	return fp->integral + ((float)fp->fractional / (1 << 16));
}
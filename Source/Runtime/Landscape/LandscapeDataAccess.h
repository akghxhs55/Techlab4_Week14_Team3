#pragma once

#define LANDSCAPE_ZSCALE (1.0f / 128.0f)
#define LANDSCAPE_INV_ZSCALE (128.0f)

namespace LandscapeDataAccess
{
	constexpr int32 MaxValue = 65535;
	constexpr float MidValue = 32768.0f;

	inline float GetLocalHeight(uint16 Height)
	{
		return (static_cast<float>(Height) - MidValue) * LANDSCAPE_ZSCALE;
	}

	inline uint16 GetTexHeight(float Height)
	{
		const float V = Height * LANDSCAPE_INV_ZSCALE + MidValue;
		return static_cast<uint16>(FMath::Clamp(V, 0.0f, static_cast<float>(MaxValue)) + 0.5f);
	}
}

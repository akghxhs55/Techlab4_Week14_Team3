#pragma once

struct FDepthOfFieldSettings
{
	bool bEnabled = false;
	float FocalDistance = 500.0f; // scene units (centimeters)
	float ApertureFStop = 4.0f;
	float MinimumFStop = 1.2f;
	int DiaphragmBladeCount = 5;
	float SensorWidthMM = 36.0f;
	float DepthBlurKmFor50Percent = 1.0f;
	float DepthBlurRadius = 0.0f; // pixels at 1080p
	// 근경과 원경 블러 강도 및 최대 반경
	float NearBlurScale = 1.0f;
	float FarBlurScale = 1.0f;
	float NearMaxBlurRadius = 20.0f;
	float FarMaxBlurRadius = 20.0f;
};

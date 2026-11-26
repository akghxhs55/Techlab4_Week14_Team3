#pragma once

struct FPhysicsSettings
{
	float DefaultGravityZ = -9.81f;
	float MaxPhysicsDeltaTime = 1.0f / 30.0f;
	bool bSubstepping = false;
	float MaxSubstepDeltaTime = 1.0f / 60.0f;
	int32 MaxSubsteps = 6;
};

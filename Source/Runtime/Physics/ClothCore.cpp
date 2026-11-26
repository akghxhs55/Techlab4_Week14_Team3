#include "EnginePCH.h"
#include "Physics/ClothCore.h"

namespace
{
	void ForwardNvClothLog(int32 Level, const char* Message)
	{
		if (!Message)
		{
			return;
		}
		if (Level == 0)
		{
			KRS_LOG(Info, "[NvCloth] {}", Message);
		}
		else if (Level == 1)
		{
			KRS_LOG(Warning, "[NvCloth] {}", Message);
		}
		else
		{
			KRS_LOG(Error, "[NvCloth] {}", Message);
		}
	}
}

bool FClothCore::Init()
{
	if (IsInitialized())
	{
		return true;
	}

	bBridgeInitialized = ClothBridge_Initialize(&ForwardNvClothLog);
	if (!bBridgeInitialized)
	{
		KRS_LOG(Error, "[NvCloth] Failed to initialize bridge");
		return false;
	}

	Factory = ClothBridge_CreateFactoryCPU();
	if (!Factory)
	{
		KRS_LOG(Error, "[NvCloth] Failed to create CPU factory");
		Shutdown();
		return false;
	}

	KRS_LOG(Info, "[NvCloth] Initialized CPU factory");
	return true;
}

void FClothCore::Shutdown()
{
	if (Factory)
	{
		ClothBridge_DestroyFactory(Factory);
		Factory = nullptr;
	}

	if (bBridgeInitialized)
	{
		ClothBridge_Shutdown();
		bBridgeInitialized = false;
	}
}

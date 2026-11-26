#pragma once

#include "ClothBridge.h"

class FClothCore
{
public:
	static bool Init();
	static void Shutdown();

	static bool IsInitialized() { return Factory != nullptr; }
	static FClothFactoryHandle GetFactory() { return Factory; }

private:
	inline static FClothFactoryHandle Factory = nullptr;
	inline static bool bBridgeInitialized = false;
};

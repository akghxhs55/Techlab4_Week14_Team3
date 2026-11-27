#include "EnginePCH.h"
#include "LandscapeProxy.h"

#include "LandscapeInfo.h"

ULandscapeInfo* ALandscapeProxy::CreateLandscapeInfo()
{
	if (LandscapeInfo)
	{
		return LandscapeInfo;
	}

	LandscapeInfo = NewObject<ULandscapeInfo>(this);
	LandscapeInfo->LandscapeActor = GetLandscapeActor();
	LandscapeInfo->ComponentSizeQuads = ComponentSizeQuads;
	return LandscapeInfo;
}

void ALandscapeProxy::RegisterAllComponents()
{
	CreateLandscapeInfo();
	Super::RegisterAllComponents();
}

void ALandscapeProxy::UnregisterAllComponents()
{
	Super::UnregisterAllComponents();
	delete LandscapeInfo;
	LandscapeInfo = nullptr;
}

#include "EnginePCH.h"
#include "LandscapeInfo.h"

#include "Landscape.h"
#include "LandscapeComponent.h"
#include "LandscapeHeightfieldCollisionComponent.h"

FIntPoint ULandscapeInfo::GetComponentKey(int32 SectionBaseX, int32 SectionBaseY) const
{
	assert(ComponentSizeQuads > 0);
	return { SectionBaseX / ComponentSizeQuads, SectionBaseY / ComponentSizeQuads };
}

ALandscapeProxy* ULandscapeInfo::GetLandscapeProxy() const
{
	return LandscapeActor;
}

void ULandscapeInfo::RegisterActorComponent(ULandscapeComponent* Component)
{
	if (!Component)
	{
		return;
	}

	const FIntPoint Key = GetComponentKey(Component->SectionBaseX, Component->SectionBaseY);
	ULandscapeComponent* Existing = XYtoComponentMap.FindRef(Key);
	assert((!Existing || Existing == Component) && "Component already registered");
	
	XYtoComponentMap.Add(Key, Component);
}

void ULandscapeInfo::UnregisterActorComponent(const ULandscapeComponent* Component)
{
	if (!Component)
	{
		return;
	}


	const FIntPoint Key = GetComponentKey(Component->SectionBaseX, Component->SectionBaseY);
	if (XYtoComponentMap.FindRef(Key) == Component)
	{
		XYtoComponentMap.Remove(Key);
	}
}

void ULandscapeInfo::RegisterCollisionComponent(ULandscapeHeightfieldCollisionComponent* Component)
{
	if (!Component)
	{
		return;
	}

	const FIntPoint Key = GetComponentKey(Component->SectionBaseX, Component->SectionBaseY);
	ULandscapeHeightfieldCollisionComponent* Existing = XYtoCollisionComponentMap.FindRef(Key);
	assert((!Existing || Existing == Component) && "Component already registered");

	XYtoCollisionComponentMap.Add(Key, Component);
}

void ULandscapeInfo::UnregisterCollisionComponent(const ULandscapeHeightfieldCollisionComponent* Component)
{
	if (!Component)
	{
		return;
	}


	const FIntPoint Key = GetComponentKey(Component->SectionBaseX, Component->SectionBaseY);
	if (XYtoCollisionComponentMap.FindRef(Key) == Component)
	{
		XYtoCollisionComponentMap.Remove(Key);
	}
}

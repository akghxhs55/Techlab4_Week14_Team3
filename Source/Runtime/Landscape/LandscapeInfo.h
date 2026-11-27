#pragma once

#include "LandscapeInfo.generated.h"

class ULandscapeComponent;
class ULandscapeHeightfieldCollisionComponent;
class ALandscapeProxy;
class ALandscape;

UCLASS()
class ULandscapeInfo : public UObject
{
	GENERATED_BODY()

public:
	ALandscape* LandscapeActor = nullptr;
	int32 ComponentSizeQuads = 0;

	TMap<FIntPoint, ULandscapeComponent*> XYtoComponentMap;
	TMap<FIntPoint, ULandscapeHeightfieldCollisionComponent*> XYtoCollisionComponentMap;

	FIntPoint GetComponentKey(int32 SectionBaseX, int32 SectionBaseY) const;

	ALandscapeProxy* GetLandscapeProxy() const;

	void RegisterActorComponent(ULandscapeComponent* Component);
	void UnregisterActorComponent(const ULandscapeComponent* Component);

	void RegisterCollisionComponent(ULandscapeHeightfieldCollisionComponent* Component);
	void UnregisterCollisionComponent(const ULandscapeHeightfieldCollisionComponent* Component);
};

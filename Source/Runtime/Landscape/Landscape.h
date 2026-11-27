#pragma once

#include "LandscapeProxy.h"
#include "Landscape.generated.h"

UCLASS()
class ALandscape : public ALandscapeProxy
{
	GENERATED_BODY()

public:
	ALandscape* GetLandscapeActor() override { return this; }
};

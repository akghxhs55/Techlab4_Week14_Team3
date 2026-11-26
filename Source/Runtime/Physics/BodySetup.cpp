#include "EnginePCH.h"
#include "BodySetup.h"

#include "PhysicsAsset.h"
#include "PhysicsCore.h"
#include "UObject/Casts.h"
#include <geometry/PxTriangleMesh.h>

UBodySetup::~UBodySetup()
{
	if (TriangleMesh && FPhysicsCore::IsInitialized())
	{
		TriangleMesh->release();
	}
	TriangleMesh = nullptr;

	if (UPhysicsAsset* Asset = Cast<UPhysicsAsset>(GetOuter()))
	{
		Asset->BodySetups.Remove(this);
	}
}

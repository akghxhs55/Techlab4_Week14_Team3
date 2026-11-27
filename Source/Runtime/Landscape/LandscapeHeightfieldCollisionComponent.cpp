#include "EnginePCH.h"
#include "LandscapeHeightfieldCollisionComponent.h"

#include <geometry/PxHeightField.h>
#include "LandscapeComponent.h"
#include "LandscapeInfo.h"
#include "LandscapeProxy.h"

void PxHeightFieldDeleter::operator()(physx::PxHeightField* Heightfield) const
{
	if (Heightfield)
	{
		Heightfield->release();
	}
}

float ULandscapeHeightfieldCollisionComponent::CollisionScale() const
{
	if (!RenderComponentRef || CollisionSizeQuads <= 0)
	{
		return 1.0f;
	}

	return static_cast<float>(RenderComponentRef->ComponentSizeQuads) / static_cast<float>(CollisionSizeQuads);
}

void ULandscapeHeightfieldCollisionComponent::CreateCollisionObject()
{
	// TODO: 물리 (CollisionHeightData로부터 HeightfieldRef 생성)
}

ULandscapeInfo* ULandscapeHeightfieldCollisionComponent::GetLandscapeInfo() const
{
	ALandscapeProxy* Proxy = GetLandscapeProxy();
	return Proxy ? Proxy->GetLandscapeInfo() : nullptr;
}

ALandscapeProxy* ULandscapeHeightfieldCollisionComponent::GetLandscapeProxy() const
{
	return Cast<ALandscapeProxy>(GetOwner());
}

void ULandscapeHeightfieldCollisionComponent::OnRegister()
{
	Super::OnRegister();
	if (ULandscapeInfo* Info = GetLandscapeInfo())
	{
		Info->RegisterCollisionComponent(this);
	}
}

void ULandscapeHeightfieldCollisionComponent::OnUnregister()
{
	if (ULandscapeInfo* Info = GetLandscapeInfo())
	{
		Info->UnregisterCollisionComponent(this);
	}
	Super::OnUnregister();
}

void ULandscapeHeightfieldCollisionComponent::OnCreatePhysicsState()
{
	// TODO: 물리
	Super::OnCreatePhysicsState();
}

void ULandscapeHeightfieldCollisionComponent::OnDestroyPhysicsState()
{
	// TODO: 물리
	Super::OnDestroyPhysicsState();
}

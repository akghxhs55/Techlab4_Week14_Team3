#include "EnginePCH.h"
#include "LandscapeComponent.h"
#include "LandscapeRender.h"

#include "LandscapeInfo.h"
#include "LandscapeProxy.h"
#include "Render/Texture2D.h"

ULandscapeComponent::~ULandscapeComponent()
{
	delete HeightmapTexture;
	HeightmapTexture = nullptr;
}

ULandscapeInfo* ULandscapeComponent::GetLandscapeInfo() const
{
	ALandscapeProxy* Proxy = GetLandscapeProxy();
	return Proxy ? Proxy->GetLandscapeInfo() : nullptr;
}

ALandscapeProxy* ULandscapeComponent::GetLandscapeProxy() const
{
	return Cast<ALandscapeProxy>(GetOwner());
}

FPrimitiveSceneProxy* ULandscapeComponent::CreateSceneProxy()
{
	if (!HeightmapTexture || ComponentSizeQuads <= 0)
	{
		return UPrimitiveComponent::CreateSceneProxy();
	}
	return new FLandscapeSceneProxy(this);
}

void ULandscapeComponent::OnRegister()
{
	Super::OnRegister();
	if (ULandscapeInfo* Info = GetLandscapeInfo())
	{
		Info->RegisterActorComponent(this);
	}
}

void ULandscapeComponent::OnUnregister()
{
	if (ULandscapeInfo* Info = GetLandscapeInfo())
	{
		Info->UnregisterActorComponent(this);
	}
	Super::OnUnregister();
}

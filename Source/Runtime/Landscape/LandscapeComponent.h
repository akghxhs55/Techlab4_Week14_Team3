#pragma once

#include "LandscapeHeightfieldCollisionComponent.h"
#include "Components/PrimitiveComponent.h"
#include "LandscapeComponent.generated.h"

class ULandscapeInfo;
class ALandscapeProxy;
class UTexture2D;

UCLASS()
class ULandscapeComponent : public UPrimitiveComponent
{
	GENERATED_BODY()

public:
	UPROPERTY()
	int32 SectionBaseX = 0;
	UPROPERTY()
	int32 SectionBaseY = 0;
	UPROPERTY()
	int32 ComponentSizeQuads = 0;
	UPROPERTY()
	int32 CollisionMipLevel = 0;

	// (X,Y): UV Scale, (Z,W): UV Offset
	UPROPERTY()
	FVector4 HeightmapScaleBias = FVector4(1.0f, 1.0f, 0.0f, 0.0f);

	FBox CachedLocalBox;

	FIntPoint GetSectionBase() const { return { SectionBaseX, SectionBaseY }; }

	ULandscapeHeightfieldCollisionComponent* GetCollisionComponent() const { return CollisionComponentRef; }
	void SetCollisionComponent(ULandscapeHeightfieldCollisionComponent* InCollisionComponent) { CollisionComponentRef = InCollisionComponent; }

	UTexture2D* GetHeightmap() const { return HeightmapTexture; }
	void SetHeightmapTexture(UTexture2D* InHeightmapTexture) { HeightmapTexture = InHeightmapTexture; }

	ULandscapeInfo* GetLandscapeInfo() const;
	ALandscapeProxy* GetLandscapeProxy() const;

	FBox CalcLocalBounds() const override { return CachedLocalBox; }

	FPrimitiveSceneProxy* CreateSceneProxy() override;
	
protected:
	void OnRegister() override;
	void OnUnregister() override;

private:
	UPROPERTY()
	ULandscapeHeightfieldCollisionComponent* CollisionComponentRef = nullptr;
	UPROPERTY(VisibleAnywhere)
	UTexture2D* HeightmapTexture = nullptr;
};

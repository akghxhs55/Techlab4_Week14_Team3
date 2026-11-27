#pragma once

#include "GameFramework/Actor.h"
#include "LandscapeProxy.generated.h"

class ULandscapeHeightfieldCollisionComponent;
class ULandscapeComponent;
class ALandscape;
class ULandscapeInfo;
class UMaterialInterface;

UCLASS()
class ALandscapeProxy : public AActor
{
	GENERATED_BODY()

public:
	UPROPERTY(VisibleAnywhere)
	int32 ComponentSizeQuads = 63; // 2^n - 1
	UPROPERTY(VisibleAnywhere)
	int32 CollisionMipLevel = 0;
	UPROPERTY(EditAnywhere)
	UMaterialInterface* LandscapeMaterial = nullptr;

	UPROPERTY()
	TArray<ULandscapeComponent*> LandscapeComponents;
	UPROPERTY()
	TArray<ULandscapeHeightfieldCollisionComponent*> CollisionComponents;

	virtual ALandscape* GetLandscapeActor() = 0;
	ULandscapeInfo* GetLandscapeInfo() const { return LandscapeInfo; }
	ULandscapeInfo* CreateLandscapeInfo();

	// 높이 데이터로 컴포넌트, 텍스처, 콜리전 데이터를 생성
	void Import(int32 MinX, int32 MinY, int32 MaxX, int32 MaxY, const TArray<uint16>& HeightData);

	void RegisterAllComponents() override;
	void UnregisterAllComponents() override;

	ULandscapeInfo* LandscapeInfo = nullptr;
};

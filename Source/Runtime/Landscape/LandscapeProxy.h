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
	
	UPROPERTY(EditAnywhere)
	float LOD0ScreenSize = 0.5f;          // 화면 크기가 이 값 이상이면 LOD0
	UPROPERTY(EditAnywhere)
	float LOD0DistributionSetting = 1.25f; // LOD0 → LOD1 경계 = LOD0ScreenSize / 이 값
	UPROPERTY(EditAnywhere)
	float LODDistributionSetting = 3.0f;   // LOD1 이후 화면 크기가 이 배수만큼 줄 때마다 LOD 

	UPROPERTY()
	TArray<ULandscapeComponent*> LandscapeComponents;
	UPROPERTY()
	TArray<ULandscapeHeightfieldCollisionComponent*> CollisionComponents;

	ULandscapeInfo* LandscapeInfo = nullptr;

	virtual ALandscape* GetLandscapeActor() = 0;
	ULandscapeInfo* GetLandscapeInfo() const { return LandscapeInfo; }
	ULandscapeInfo* CreateLandscapeInfo();

	// 높이 데이터로 컴포넌트, 텍스처, 콜리전 데이터를 생성
	void Import(int32 MinX, int32 MinY, int32 MaxX, int32 MaxY, const TArray<uint16>& HeightData);

	using Super::Serialize;
	virtual void Serialize(FStructuredArchive::FRecord Record) override;

	void RegisterAllComponents() override;
	void UnregisterAllComponents() override;

private:
	void DestroyLandscapeInfo();
	void RebuildComponentReferences();
	void BuildDerivedData();
	void ClearLandscapeComponents();
};

#pragma once

#include "GameFramework/Character.h"
#include "StockingActor.generated.h"

class USkeletalMesh;
class UStockingComponent;
class UStockingInteractionComponent;
class UStockingRegion;

UCLASS()
class AStockingActor : public ACharacter
{
	GENERATED_BODY()

public:
	AStockingActor();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;
	virtual void PostEditChangeProperty(const FProperty& Property) override;

	UStockingComponent* GetStockingComponent() const { return StockingComponent; }
	UStockingInteractionComponent* GetStockingInteractionComponent() const { return StockingInteractionComponent; }
	UStockingRegion* GetStockingRegion() const { return StockingRegion; }

	bool RebuildStockingFromSourceMesh();
	void ApplyStockingTearingSettings();

	UPROPERTY(EditAnywhere)
	FString SourceMeshPathHint = "female_animation";

	UPROPERTY(EditAnywhere)
	FString StockingRegionPath;

	UPROPERTY(EditAnywhere)
	bool bRagdollOnBeginPlay = true;

	UPROPERTY(EditAnywhere)
	bool bEnableImpactTearing = true;

	UPROPERTY(EditAnywhere)
	float ImpactTearImpulseThreshold = 1.5f;

	UPROPERTY(EditAnywhere)
	float ImpactScrapeSensitivity = 0.35f;

	UPROPERTY(EditAnywhere)
	float ImpactTearCooldownDuration = 0.14f;

	UPROPERTY(EditAnywhere)
	float AutoLegMaxRadiusRatio = 0.072f;

	UPROPERTY(EditAnywhere)
	float AutoLegMaxHeightRatio = 0.84f;

private:
	USkeletalMesh* ResolveSourceMeshFromHint() const;

	UStockingComponent* StockingComponent = nullptr;
	UStockingInteractionComponent* StockingInteractionComponent = nullptr;
	UStockingRegion* StockingRegion = nullptr;
	USkeletalMesh* BuiltSourceMesh = nullptr;
	bool bRagdollStartAttempted = false;
};

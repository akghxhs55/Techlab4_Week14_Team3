#pragma once

#include "Components/ActorComponent.h"
#include "Components/StockingComponent.h"
#include "Collision/Ray.h"
#include "StockingInteractionComponent.generated.h"

struct FStockingHitInfo
{
	UStockingComponent* HitComponent = nullptr;
	uint32 TriangleIndex = 0;
	FVector Barycentric = FVector(1.0f, 0.0f, 0.0f);
	FVector WorldLocation = FVector::ZeroVector;
	FVector LocalLocation = FVector::ZeroVector;
	FVector WorldNormal = FVector(0.0f, 0.0f, 1.0f);
	float Distance = 0.0f;
};

UCLASS()
class UStockingInteractionComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UStockingInteractionComponent();
	virtual ~UStockingInteractionComponent() override = default;

	virtual void TickComponent(float DeltaTime) override;

	void SetTargetStockingComponent(UStockingComponent* InStocking) { TargetStockingComponent = InStocking; }
	UStockingComponent* GetTargetStockingComponent() const { return TargetStockingComponent; }

	// 월드 광선으로 스타킹 표면 교차점과 삼각형 탐색
	bool RaycastStocking(const FRay& WorldRay, FStockingHitInfo& OutHit) const;
	static bool RaycastSingleStocking(UStockingComponent* Stocking, const FRay& WorldRay, float MaxDistance, FStockingHitInfo& OutHit);

	// 에디터 및 런타임 공용 집어 당기기 제어
	bool BeginGrabFromRay(const FRay& WorldRay);
	void UpdateGrabFromRay(const FRay& WorldRay);
	void EndGrab();

	bool IsGrabbing() const { return bIsGrabbing && ActiveStocking != nullptr; }
	UStockingComponent* GetActiveStocking() const { return ActiveStocking; }
	const FVector& GetGrabWorldOrigin() const { return DragPlanePoint; }
	const FVector& GetCurrentDragWorldPosition() const { return CurrentDragWorldPosition; }

	UPROPERTY(EditAnywhere)
	UStockingComponent* TargetStockingComponent = nullptr;

	UPROPERTY(EditAnywhere)
	bool bUseGameViewportInput = true;

	UPROPERTY(EditAnywhere)
	bool bSearchAllWorldStockings = true;

	UPROPERTY(EditAnywhere)
	float MaxTraceDistance = 100000.0f;

private:
	bool BuildGameViewportRay(FRay& OutWorldRay) const;
	UStockingComponent* ResolveOwnerStocking() const;

	bool bIsGrabbing = false;
	UStockingComponent* ActiveStocking = nullptr;
	FVector DragPlanePoint = FVector::ZeroVector;
	FVector DragPlaneNormal = FVector(1.0f, 0.0f, 0.0f);
	FVector CurrentDragWorldPosition = FVector::ZeroVector;
};

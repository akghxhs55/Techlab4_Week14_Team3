#pragma once

#include "Components/PrimitiveComponent.h"
#include "LandscapeHeightfieldCollisionComponent.generated.h"

class ULandscapeComponent;
class ULandscapeInfo;
class ALandscapeProxy;
namespace physx { class PxHeightField; }

struct PxHeightFieldDeleter
{
	void operator()(physx::PxHeightField* HeightField) const;
};

UCLASS()
class ULandscapeHeightfieldCollisionComponent : public UPrimitiveComponent
{
	GENERATED_BODY()

public:
	UPROPERTY()
	int32 SectionBaseX = 0;
	UPROPERTY()
	int32 SectionBaseY = 0;
	UPROPERTY()
	int32 CollisionSizeQuads = 0;
	FBox CachedLocalBox;

	TArray<uint16> CollisionHeightData;

	ULandscapeComponent* GetRenderComponent() const { return RenderComponentRef; }
	void SetRenderComponent(ULandscapeComponent* InRenderComponent) { RenderComponentRef = InRenderComponent; }

	// 언리얼에서는 멤버 변수지만 파생 값이니 함수로 제공
	float CollisionScale() const;

	void CreateCollisionObject();

	ULandscapeInfo* GetLandscapeInfo() const;
	ALandscapeProxy* GetLandscapeProxy() const;

	FBox CalcLocalBounds() const override { return CachedLocalBox; }

protected:
	void OnRegister() override;
	void OnUnregister() override;
	void OnCreatePhysicsState() override;
	void OnDestroyPhysicsState() override;

private:
	UPROPERTY()
	ULandscapeComponent* RenderComponentRef = nullptr;

	// TUniquePtr에는 deleter를 지정할 수 없으므로 std::unique_ptr을 사용
	std::unique_ptr<physx::PxHeightField, PxHeightFieldDeleter> HeightfieldRef;
};

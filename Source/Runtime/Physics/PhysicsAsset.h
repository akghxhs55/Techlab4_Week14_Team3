#pragma once

#include "ConstraintInstance.h"
#include "PhysicsAsset.generated.h"

class USkeletalMesh;
class UBodySetup;
struct FBoneInfo;

USTRUCT()
struct FPhysicsAssetConstraint
{
	GENERATED_BODY()

	UPROPERTY()
	FString ChildBone;

	UPROPERTY()
	FString ParentBone;

	UPROPERTY()
	EConstraintMotion Swing1Motion = EConstraintMotion::Limited;
	
	UPROPERTY()
	EConstraintMotion Swing2Motion = EConstraintMotion::Limited;

	UPROPERTY()
	EConstraintMotion TwistMotion = EConstraintMotion::Limited;

	UPROPERTY()
	float Swing1LimitDeg = 45.0f;

	UPROPERTY()
	float Swing2LimitDeg = 45.0f;

	UPROPERTY()
	float TwistMinDeg = -45.0f;

	UPROPERTY()
	float TwistMaxDeg = 45.0f;

	UPROPERTY()
	FRotator FrameOffset = FRotator::Identitiy;
};

UCLASS()
class UPhysicsAsset : public UObject
{
	GENERATED_BODY()

public:
	virtual ~UPhysicsAsset() override;

	using Super::Serialize;
	void Serialize(FStructuredArchive::FRecord Record) override;

	bool Validate(const USkeletalMesh& Mesh, const FString& DebugName) const;
	bool Validate(const TArray<FBoneInfo>& Bones, const FString& DebugName) const;

	void ClearBodies();
	const FString& GetAssetPath() const { return AssetPath; }
	void SetAssetPath(const FString& Path) { AssetPath = Path; }

	// 독립 Physics Asset 파일이 미리보기에 사용할 스켈레탈 메시 에셋 키.
	FString PreviewMeshPath;

	TArray<UBodySetup*> BodySetups;

	UPROPERTY()
	TArray<FPhysicsAssetConstraint> Constraints;

private:
	FString AssetPath;
};

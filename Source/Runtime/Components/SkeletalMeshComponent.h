#pragma once

#include "SkinnedMeshComponent.h"
#include "Animation/AnimInstance.h"
#include <functional>
#include "SkeletalMeshComponent.generated.h"

struct FConstraintInstance;
struct FBodyInstance;
struct FPhysicsContactImpact;
struct FHitResult;
struct FRay;
class UAnimInstance;
class UAnimSequence;
class UBlendSpace1D;
class UWorld;
class FLineBatcher;
class UPhysicsAsset;
namespace physx
{
	class PxAggregate;
}

struct FSkeletalPhysicsImpact
{
	int32 BoneIndex = -1;
	FVector WorldLocation = FVector::ZeroVector;
	FVector WorldNormal = FVector(0.0f, 0.0f, 1.0f);
	FVector WorldImpulse = FVector::ZeroVector;
	FVector WorldSlideVelocity = FVector::ZeroVector;
	float ImpulseMagnitude = 0.0f;
	bool bFromExternalImpulse = false;
};

using FSkeletalPhysicsImpactCallback = std::function<void(const FSkeletalPhysicsImpact&)>;

// World의 SkeletalMeshComponent마다 root 디버그를 그린다 (DrawCollisionShapes와 같은 방식). PIE의 Show Root에서 호출
void DrawSkeletalRootDebug(UWorld* World, FLineBatcher* LineBatcher);
// Settings > Debug Draw (PIE) > Show Root 값. 에디터가 매 프레임 갱신하고, 런타임 액터("root" 라벨 등)가 읽는다
extern bool GShowSkeletalRootDebug;
void DrawPhysicsAssetBodies(UWorld* World, FLineBatcher* LineBatcher);
class UAnimationAsset;
class UAnimSingleNodeInstance;
class UAnimGraphInstance;

class USkeletalMeshComponent;

struct FSkeletalMeshComponentEndPhysicsTickFunction : public FTickFunction
{
	USkeletalMeshComponent* Target = nullptr;
	virtual void ExecuteTick(float DeltaTime) override;
};

UCLASS()
class USkeletalMeshComponent : public USkinnedMeshComponent
{
	GENERATED_BODY()

public:
    USkeletalMeshComponent();
    ~USkeletalMeshComponent() override;
    bool SetAnimBlueprint(const FString& Path);
    bool ReloadAnimBlueprint();
    UAnimGraphInstance* CreateAnimGraph();
    const FString& GetAnimBlueprintPath() const;
    const FString& GetAnimBlueprintError() const;
	void SetAnimInstance(UAnimInstance* InInstance);
	UAnimSingleNodeInstance* GetSingleNodeInstance() const;
	UAnimInstance* GetAnimInstance() const { return AnimInstance; }
	void BeginPlay() override;
	void TickComponent(float DeltaTime) override;

	virtual void RegisterComponentTickFunctions(bool bRegister) override;
	void EndPhysicsTickComponent(float DeltaTime);

	virtual void SetSimulatePhysics(bool bSimulate) override;
	// 지정 본과 그 하위 본의 바디만 동적으로 만들고, 나머지는 키네마틱으로 유지한다.
	bool SetSimulatePhysicsBelow(int32 RootBoneIndex);
	virtual void AddImpulse(FVector Impulse, FName BoneName = NAME_None, bool bVelChange = false) override;
	void AddImpulseToAllBodiesBelow(FVector Impulse, FName BoneName = NAME_None, bool bVelChange = false, bool bIncludeSelf = true);
	virtual void OnTransformDirty(ETeleportType TeleportType) override;

	void InitArticulated(FPhysScene* PhysScene, const UPhysicsAsset* OverrideAsset = nullptr);
	void TermArticulated();
	void UpdateKinematicBonesToAnim(ETeleportType TeleportType);
	void BlendInPhysics();

	void SetPhysicsAsset(UPhysicsAsset* InAsset);
	UPhysicsAsset* GetPhysicsAsset() const;
	bool StartRagdoll();
	void StopRagdoll();
	bool SetRagdollSimulationEnabled(bool bEnable);
	bool IsRagdollActive() const;

	UPROPERTY(EditAnywhere)
	bool bStartRagdollOnBeginPlay = false;

	FBodyInstance* GetBodyInstance(const FString& BoneName) const;
	void AddImpulseToBone(const FString& BoneName, const FVector& Impulse, bool bVelocityChange = false);
	void SetRagdollLinearVelocity(const FVector& Velocity);
	bool IsAnyBodySimulatingPhysics() const;
	void AddPhysicsImpactListener(const void* ListenerKey, FSkeletalPhysicsImpactCallback Callback);
	void RemovePhysicsImpactListener(const void* ListenerKey);
	void NotifyPhysicsContactImpact(const FPhysicsContactImpact& ContactImpact);
	int32 FindBoneIndexForBody(const FBodyInstance* Body) const;
	bool HasArticulatedBodies() const;
	bool RaycastPhysicsBodies(const FRay& WorldRay, FHitResult& OutHit) const;
	bool SweepPhysicsBodies(const FRay& WorldRay, float MaxDistance, float Radius, FHitResult& OutHit) const;

	const TArray<FBoneInfo>& GetBones() const;
	int32 FindBoneIndex(const FString& Name) const;
	FTransform GetBoneLocalTransform(int32 BoneIndex) const;
	void SetBoneLocalTransform(int32 BoneIndex, const FTransform& Local);
	const TArray<FTransform>& GetBoneSpaceTransforms() const { return BoneSpaceTransforms; }
	void SetBoneSpaceTransforms(const struct FCompactPose& Pose); // 애니메이션 평가 결과를 받는 입구
	FMatrix GetBoneComponentMatrix(int32 BoneIndex) const;
	FMatrix GetBoneWorldMatrix(int32 BoneIndex) const;

	bool DoesSocketExist(const FString& InSocketName) const;
	FMatrix GetSocketWorldMatrix(const FString& InSocketName) const;
	FQuat GetSocketWorldRotationQuat(const FString& InSocketName) const;
	FVector GetSocketWorldScale3D(const FString& InSocketName) const;
	FTransform GetSocketTransform(const FString& InSocketName) const;
	void GetSocketNames(TArray<FString>& OutSocketNames) const;

	void ResetPose();
	void RefreshBoneTransformsNow();

	using Super::Serialize;
	virtual void Serialize(FStructuredArchive::FRecord Record) override;

	void SetAnimation(UAnimationAsset* NewAnimToPlay);
	void Play(bool bLooping = true);
	void Stop();
	bool IsPlaying() const;
	void PlayAnimation(UAnimationAsset* NewAnimToPlay, bool bLoop = true);
	void RefreshAnimationPose();

	UPROPERTY()
	float BlendSpaceInput = 300.0f;

	UPROPERTY()
	float BlendSpaceInputY = 0.0f;

	// 루트 모션: 이번 Tick까지 모인 이동량(월드 공간). CharacterMovement가 가져가 Actor를 움직인다
	bool HasPendingRootMotion() const { return bHasPendingRootMotion; }
	FTransform ConsumeRootMotion();

	// root 부모 공간 delta → 월드 공간 delta (Unreal ConvertLocalRootMotionToWorld + Armature 변환). 시퀀스 뷰어 미리보기도 쓴다
	FTransform ConvertRootMotionToWorld(const FTransform& ParentSpaceDelta) const;

	// 루트 모션이 뽑히는 본. "root"를 먼저, 없으면 첫 본 (UAnimSequence::FindRootTrack과 같은 규칙)
	int32 GetRootMotionBoneIndex() const;

	// 디버그: root 본 구체 + 액터 위치(root 높이)에서 root까지의 선
	void DrawRootDebug(FLineBatcher* LineBatcher) const;

	// 디버그: 메시의 Physics 에셋 바디(구·박스·캡슐)를 각 본의 현재 월드 행렬 위에 그린다
	void DrawPhysicsAssetDebug(FLineBatcher* LineBatcher) const;

	void StartPreviewDissolve(float InDuration, const FVector4& InEdgeColor);
	void UpdatePreviewDissolve(float DeltaTime);
	void ResetPreviewDissolve();
	void SetDissolveHidden(bool bHidden);
	bool IsDissolveHidden() const { return bDissolveHidden; }
	bool IsPreviewDissolveActive() const { return bPreviewDissolveActive; }

protected:
	virtual void OnCreatePhysicsState() override;
	virtual void OnDestroyPhysicsState() override;

	// 단일 애니메이션 에셋 경로/루프/재생 속도를 씬에 저장·복원한다 (Serialize에서 부른다)
	void SerializeSingleAnimation(FStructuredArchive::FRecord& Record);

	void RefreshBoneTransforms() override;
	void OnSkeletalMeshChanged() override;

private:
	FBodyInstance* FindArticulatedBody(const FString& BoneName) const;
	void InitConstraints(const UPhysicsAsset& Asset);
	bool IsSimulatingRagdoll() const;
	void UpdateComponentToRootBody();

	struct FPhysicsImpactListenerEntry
	{
		const void* Key = nullptr;
		FSkeletalPhysicsImpactCallback Callback;
	};
	UPhysicsAsset* PhysicsAssetOverride = nullptr;
	TArray<FPhysicsImpactListenerEntry> PhysicsImpactListeners;

    UPROPERTY()
    FString AnimBlueprintPath;

    TUniquePtr<UAnimGraphInstance> ManagedAnimGraph;
    FString AnimBlueprintError;
    bool bBlueprintReloadRequested = false;
    bool bUpdatingAnimation = false;

	UPROPERTY()
	UAnimInstance* AnimInstance = nullptr;

	TArray<FTransform> BoneSpaceTransforms;

	FTransform PendingRootMotion;           // 월드 공간 누적값
	bool bHasPendingRootMotion = false;

	FSkeletalMeshComponentEndPhysicsTickFunction EndPhysicsTickFunction;
	TArray<TUniquePtr<FBodyInstance>> Bodies;
	physx::PxAggregate* Aggregate = nullptr;
	TArray<FBodyInstance*> BoneToBody;
	bool bPendingKinematicTeleport = false;
	bool bSimulatingBodiesBelow = false;
	TArray<TUniquePtr<FConstraintInstance>> Constraints;

	bool bPreviewDissolveActive = false;
	bool bDissolveHidden = false;
	float PreviewDissolveDuration = 1.45f;
	float PreviewDissolveElapsed = 0.0f;
	TArray<UMaterialInterface*> PreviewDissolveSavedMaterials;
	TArray<UMaterialInstanceDynamic*> PreviewDissolveMaterials;
};

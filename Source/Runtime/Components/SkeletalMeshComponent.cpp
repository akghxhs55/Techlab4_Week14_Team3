#include "EnginePCH.h"
#include "SkeletalMeshComponent.h"

#include <PxPhysics.h>
#include <PxScene.h>
#include <foundation/PxMathUtils.h>
#include "Serialization/MathSerialization.h"
#include "Animation/BonePose.h"
#include "Animation/AnimNodeBase.h"
#include "Animation/AnimSingleNodeInstance.h"
#include "Animation/AnimGraphInstance.h"
#include "Animation/AnimBlueprintCompiler.h"
#include "Animation/AnimBlueprintFile.h"
#include "Asset/AssetUtils.h"
#include "Core/Misc/Paths.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/Casts.h"
#include "UObject/UObjectIterator.h"
#include "Render/LineBatcher.h"
#include "GameFramework/Actor.h"
#include "Animation/AnimSequence.h"
#include "Animation/BlendSpace1D.h"
#include "Animation/BlendSpace2D.h"
#include "Animation/BlendSpaceFile.h"
#include "Asset/AssetManager.h"
#include "Render/Material.h"
#include "Render/MaterialInstance.h"
#include "Components/ParticleSystemComponent.h"
#include "Engine/World.h"
#include "Physics/PhysicsAsset.h"
#include "Physics/BodySetup.h"
#include "Physics/PhysicsCore.h"
#include "Physics/PhysXConversion.h"
#include "Physics/PhysicsBoneUtils.h"

namespace
{
	void AddLocalArc(FLineBatcher* LineBatcher, const FMatrix& ShapeToWorld, const FVector& Center, const FVector& AxisA, const FVector& AxisB,
		float Radius, float StartRad, float EndRad, const FVector4& Color, int32 Segments = 16)
	{
		auto PointAt = [&](float T)
		{
			return ShapeToWorld.TransformPosition(Center + AxisA * (std::cos(T) * Radius) + AxisB * (std::sin(T) * Radius));
		};
		FVector Prev = PointAt(StartRad);
		for (int32 i = 1; i <= Segments; ++i)
		{
			const FVector P = PointAt(StartRad + (EndRad - StartRad) * i / Segments);
			LineBatcher->AddLine(Prev, P, Color);
			Prev = P;
		}
	}

	const FVector LocalX(1.0f, 0.0f, 0.0f);
	const FVector LocalY(0.0f, 1.0f, 0.0f);
	const FVector LocalZ(0.0f, 0.0f, 1.0f);

	void DrawLocalSphere(FLineBatcher* LineBatcher, const FMatrix& M, float R, const FVector4& Color)
	{
		const FVector O = FVector::ZeroVector;
		AddLocalArc(LineBatcher, M, O, LocalX, LocalY, R, 0.0f, 2.0f * PI, Color);
		AddLocalArc(LineBatcher, M, O, LocalX, LocalZ, R, 0.0f, 2.0f * PI, Color);
		AddLocalArc(LineBatcher, M, O, LocalY, LocalZ, R, 0.0f, 2.0f * PI, Color);
	}

	void DrawLocalCapsule(FLineBatcher* LineBatcher, const FMatrix& M, float R, float Length, const FVector4& Color)
	{
		const float H = FMath::Max(0.0f, 0.5f * Length);
		const FVector Top = LocalZ * H;
		const FVector Bottom = LocalZ * -H;

		AddLocalArc(LineBatcher, M, Top, LocalX, LocalY, R, 0.0f, 2.0f * PI, Color);
		AddLocalArc(LineBatcher, M, Bottom, LocalX, LocalY, R, 0.0f, 2.0f * PI, Color);
		AddLocalArc(LineBatcher, M, Top, LocalX, LocalZ, R, 0.0f, PI, Color);
		AddLocalArc(LineBatcher, M, Top, LocalY, LocalZ, R, 0.0f, PI, Color);
		AddLocalArc(LineBatcher, M, Bottom, LocalX, LocalZ, R, 0.0f, -PI, Color);
		AddLocalArc(LineBatcher, M, Bottom, LocalY, LocalZ, R, 0.0f, -PI, Color);
		for (const FVector& Side : { LocalX, LocalX * -1.0f, LocalY, LocalY * -1.0f })
		{
			LineBatcher->AddLine(M.TransformPosition(Top + Side * R), M.TransformPosition(Bottom + Side * R), Color);
		}
	}

	void BuildBindPoses(const TArray<FBoneInfo>& Bones, const FVector& CompScale, TArray<physx::PxTransform>& Out)
	{
		TArray<FMatrix> CS;
		CS.SetNum(Bones.Num());
		Out.SetNum(Bones.Num());

		for (int32 i = 0; i < Bones.Num(); ++i)
		{
			const FMatrix Local = Bones[i].LocalBindPose.GetLocalMatrix();
			const int32 Parent = Bones[i].ParentIndex;
			CS[i] = Parent < 0 ? Local : Local * CS[Parent];

			const FTransform T(CS[i]);
			const FVector P(T.Location.X * CompScale.X, T.Location.Y * CompScale.Y, T.Location.Z * CompScale.Z);
			Out[i] = physx::PxTransform(K2PVector(P), K2PQuat(T.Rotation.Normalized()));
		}
	}
}

void USkeletalMeshComponent::SetAnimInstance(UAnimInstance* InInstance)
{
	if (AnimInstance == InInstance)
	{
		return;
	}
	if (AnimInstance)
	{
		AnimInstance->UninitializeAnimation();
	}
    AnimInstance = InInstance;

    if (ManagedAnimGraph && ManagedAnimGraph.get() != InInstance)
    {
        ManagedAnimGraph.reset();
    }

    AnimBlueprintPath.clear();
    bBlueprintReloadRequested = false;

	if (AnimInstance)
	{
		AnimInstance->InitializeAnimation();
	}
}

void USkeletalMeshComponent::BeginPlay()
{
	Super::BeginPlay();

    if (!AnimBlueprintPath.empty() && (bBlueprintReloadRequested || !ManagedAnimGraph))
    {
        ReloadAnimBlueprint();
    }

	if (AnimInstance)
	{
		AnimInstance->InitializeAnimation();

		if (auto* Instance = GetSingleNodeInstance())
		{
			if (Instance->GetCurrentAsset())
			{
				Instance->SetPlaying(true);
			}
		}
	}
	if (IsPhysicsStateCreated() && Bodies.IsEmpty() && GetWorld())
	{
		InitArticulated(GetWorld()->GetPhysicsScene());
	}

	if (bStartRagdollOnBeginPlay && GetWorld() && GetWorld()->IsGameWorld())
	{
		StartRagdoll();
	}
}

void USkeletalMeshComponent::TickComponent(float DeltaTime)
{
    if (bBlueprintReloadRequested)
    {
        bBlueprintReloadRequested = false;
        ReloadAnimBlueprint();
    }

	if (AnimInstance && GetSkeletalMesh())
	{
        bUpdatingAnimation = true;
		if (UAnimSingleNodeInstance* Instance = Cast<UAnimSingleNodeInstance>(AnimInstance))
		{
			Instance->SetBlendSpaceInput(BlendSpaceInput);
			Instance->SetBlendSpace2DInput(BlendSpaceInput, BlendSpaceInputY);
		}

		AnimInstance->UpdateAnimation(DeltaTime);
		FPoseContext Pose(AnimInstance, GetBones());
		AnimInstance->EvaluateAnimation(Pose);
		SetBoneSpaceTransforms(Pose.Pose);
		AnimInstance->TriggerAnimNotifies(DeltaTime);

		// 루트 모션: AnimInstance에 모인 delta를 월드 공간으로 바꿔 CharacterMovement가 가져갈 때까지 쌓아 둔다
		const FRootMotionMovementParams RootMotion = AnimInstance->ConsumeExtractedRootMotion();
		if (RootMotion.bHasRootMotion)
		{
			const FTransform WorldDelta = ConvertRootMotionToWorld(RootMotion.GetRootMotionTransform());
			PendingRootMotion.Location = (bHasPendingRootMotion ? PendingRootMotion.Location : FVector::ZeroVector) + WorldDelta.Location;
			PendingRootMotion.Rotation = bHasPendingRootMotion ? (WorldDelta.Rotation * PendingRootMotion.Rotation).Normalized() : WorldDelta.Rotation;
			bHasPendingRootMotion = true;
		}
        bUpdatingAnimation = false;
	}
	UpdatePreviewDissolve(DeltaTime);
	Super::TickComponent(DeltaTime);

	UpdateKinematicBonesToAnim(bPendingKinematicTeleport ? ETeleportType::TeleportPhysics : ETeleportType::None);
	bPendingKinematicTeleport = false;
}

void USkeletalMeshComponent::RegisterComponentTickFunctions(bool bRegister)
{
	Super::RegisterComponentTickFunctions(bRegister);

	if (!bRegister)
	{
		EndPhysicsTickFunction.UnRegisterTickFunction();
		return;
	}
	
	if (IsRegistered() && GetWorld() && !EndPhysicsTickFunction.IsTickFunctionRegistered())
	{
		EndPhysicsTickFunction.RegisterTickFunction(GetWorld()->GetTickTaskManager());
	}

	bool bAnyDynamic = false;
	for (const auto& Body : Bodies) bAnyDynamic |= Body->IsDynamic();
	EndPhysicsTickFunction.SetTickFunctionEnable(bAnyDynamic);
}

void USkeletalMeshComponent::EndPhysicsTickComponent(float DeltaTime)
{
	BlendInPhysics();
}

void USkeletalMeshComponent::SetSimulatePhysics(bool bSimulate)
{
	bSimulatingBodiesBelow = false;
	for (auto& Body : Bodies)
	{
		Body->SetInstanceSimulatePhysics(bSimulate);
	}

	EndPhysicsTickFunction.SetTickFunctionEnable(bSimulate);

	if (!bSimulate)
	{
		bPendingKinematicTeleport = true;
	}
}

bool USkeletalMeshComponent::SetSimulatePhysicsBelow(int32 RootBoneIndex)
{
	const TArray<FBoneInfo>& Bones = GetBones();
	if (RootBoneIndex < 0 || RootBoneIndex >= Bones.Num()) return false;
	bSimulatingBodiesBelow = true;

	bool bAnyDynamic = false;
	for (auto& Body : Bodies)
	{
		bool bBelowRoot = false;
		// 바디가 없는 중간 본도 통과해 스켈레톤 계층 전체에서 하위 여부를 찾는다.
		int32 Bone = Body->BoneIndex;
		for (int32 Depth = 0; Bone >= 0 && Bone < Bones.Num() && Depth < Bones.Num(); ++Depth)
		{
			if (Bone == RootBoneIndex)
			{
				bBelowRoot = true;
				break;
			}
			Bone = Bones[Bone].ParentIndex;
		}
		Body->SetInstanceSimulatePhysics(bBelowRoot);
		bAnyDynamic |= Body->IsDynamic();
	}
	EndPhysicsTickFunction.SetTickFunctionEnable(bAnyDynamic);
	bPendingKinematicTeleport = true;
	return bAnyDynamic;
}

void USkeletalMeshComponent::AddImpulse(FVector Impulse, FName BoneName, bool bVelChange)
{
	FBodyInstance* Body = nullptr;
	if (BoneName.IsNone())
	{
		Body = Bodies.IsEmpty() ? nullptr : Bodies[0].get();
	}
	else
	{
		const int32 BoneIndex = FindBoneIndex(BoneName.ToString());
		Body = (BoneIndex >= 0 && BoneIndex < BoneToBody.Num()) ? BoneToBody[BoneIndex] : nullptr;
	}
	if (Body)
	{
		Body->AddImpulse(Impulse, bVelChange);
	}
}

void USkeletalMeshComponent::AddImpulseToAllBodiesBelow(FVector Impulse, FName BoneName, bool bVelChange, bool bIncludeSelf)
{
	const TArray<FBoneInfo>& Bones = GetBones();
	const int32 RootBoneIndex = BoneName.IsNone() ? 0 : FindBoneIndex(BoneName.ToString());
	if (RootBoneIndex < 0 || RootBoneIndex >= Bones.Num())
	{
		return;
	}

	for (auto& Body : Bodies)
	{
		int32 Bone = Body->BoneIndex;
		if (Bone == RootBoneIndex && !bIncludeSelf)
		{
			continue;
		}
		for (int32 Depth = 0; Bone >= 0 && Bone < Bones.Num() && Depth < Bones.Num(); ++Depth)
		{
			if (Bone == RootBoneIndex)
			{
				Body->AddImpulse(Impulse, bVelChange);
				break;
			}
			Bone = Bones[Bone].ParentIndex;
		}
	}
}

void USkeletalMeshComponent::OnTransformDirty(ETeleportType TeleportType)
{
	Super::OnTransformDirty(TeleportType);
	if (TeleportType != ETeleportType::None)
	{
		UpdateKinematicBonesToAnim(TeleportType);
	}
}

void USkeletalMeshComponent::InitArticulated(FPhysScene* PhysScene, const UPhysicsAsset* OverrideAsset)
{
	USkeletalMesh* Mesh = GetSkeletalMesh();
	const UPhysicsAsset* Asset = OverrideAsset ? OverrideAsset : GetPhysicsAsset();
	physx::PxPhysics* Physics = FPhysicsCore::GetPhysics();
	if (!Mesh || !Asset || Asset->BodySetups.IsEmpty() || !PhysScene || !PhysScene->GetPxScene() || !Physics || Aggregate)
	{
		return;
	}

	RefreshBoneTransformsNow();
	if (ComponentSpaceTransforms.Num() != Mesh->GetNumBones())
	{
		KRS_LOG(Warning, "[Ragdoll] bone transforms not ready");
		return;
	}

	physx::PxU32 NumShapes = 0;
	for (const UBodySetup* Body : Asset->BodySetups)
	{
		if (Body)
		{
			NumShapes += Body->AggGeom.GetElementCount();
		}
	}

	Aggregate = Physics->createAggregate(Asset->BodySetups.Num(), NumShapes, physx::PxGetAggregateFilterHint(physx::PxAggregateType::eGENERIC, false));
	if (!Aggregate)
	{
		return;
	}

	const FTransform LocalToWorld(GetWorldRotation(), GetWorldLocation(), GetWorldScale3D());

	BoneToBody.Empty();
	BoneToBody.SetNum(Mesh->GetNumBones());

	int32 NumFailed = 0;
	for (UBodySetup* Body : Asset->BodySetups)
	{
		const int32 BoneIndex = Body ? Mesh->FindBoneIndex(Body->BoneName) : INDEX_NONE;
		if (BoneIndex == INDEX_NONE)
		{
			NumFailed++;
			continue;
		}

		TUniquePtr<FBodyInstance> BodyInstance = MakeUnique<FBodyInstance>();
		BodyInstance->bSimulatePhysics = false;
		BodyInstance->BoneIndex = BoneIndex;
		BodyInstance->InitBody(Body, GetBoneTransform(BoneIndex, LocalToWorld), this, PhysScene, Aggregate);

		if (BodyInstance->IsValidBodyInstance())
		{
			BoneToBody[BoneIndex] = BodyInstance.get();
			Bodies.Add(std::move(BodyInstance));
		}
		else
		{
			NumFailed++;
		}
	}

	InitConstraints(*Asset);
	PhysScene->GetPxScene()->addAggregate(*Aggregate);
	KRS_LOG(Warning, "[Ragdoll] bodies created: {}, failed: {}", Bodies.Num(), NumFailed);

	if (BodyInstance.bSimulatePhysics)
	{
		SetSimulatePhysics(true);
	}
}

void USkeletalMeshComponent::TermArticulated()
{
	bSimulatingBodiesBelow = false;
	Constraints.Empty();
	for (auto& Body : Bodies)
	{
		Body->TermBody();
	}
	Bodies.Empty();

	if (Aggregate)
	{
		Aggregate->release();
		Aggregate = nullptr;
	}

	BoneToBody.Empty();
}

void USkeletalMeshComponent::UpdateKinematicBonesToAnim(ETeleportType TeleportType)
{
	if (Bodies.Num() == 0)
	{
		return;
	}

	const FTransform LocalToWorld(GetWorldRotation(), GetWorldLocation(), GetWorldScale3D());
	for (auto& Body : Bodies)
	{
		if (Body->IsDynamic())
		{
			continue;
		}
		Body->SetBodyTransform(GetBoneTransform(Body->BoneIndex, LocalToWorld), TeleportType);
	}
}

void USkeletalMeshComponent::BlendInPhysics()
{
	const TArray<FBoneInfo>& Bones = GetBones();
	if (BoneToBody.Num() != Bones.Num() || BoneSpaceTransforms.Num() != Bones.Num())
	{
		return;
	}

	if (IsSimulatingRagdoll())
	{
		UpdateComponentToRootBody();
	}

	const FMatrix WorldToComponent = GetWorldMatrix().Inverse();

	TArray<FMatrix> ComponentSpace;
	ComponentSpace.SetNum(Bones.Num());

	for (int32 i = 0; i < Bones.Num(); ++i)
	{
		const int32 Parent = Bones[i].ParentIndex;
		FBodyInstance* Body = BoneToBody[i];

		if (Body && Body->IsDynamic())
		{
			ComponentSpace[i] = Body->GetUnrealWorldTransform().GetLocalMatrix() * WorldToComponent;
			BoneSpaceTransforms[i] = (Parent < 0) ? FTransform(ComponentSpace[i]) : FTransform(ComponentSpace[i] * ComponentSpace[Parent].Inverse());
		}
		else
		{
			const FMatrix Local = BoneSpaceTransforms[i].GetLocalMatrix();
			ComponentSpace[i] = (Parent < 0) ? Local : Local * ComponentSpace[Parent];
		}
	}

	UpdateBoneTransformsNow();
}

const TArray<FBoneInfo>& USkeletalMeshComponent::GetBones() const
{
	static const TArray<FBoneInfo> Empty{};
	return GetSkeletalMesh() ? GetSkeletalMesh()->GetRefSkeleton() : Empty;
}

int32 USkeletalMeshComponent::FindBoneIndex(const FString& Name) const
{
	return GetSkeletalMesh() ? GetSkeletalMesh()->FindBoneIndex(Name) : -1;
}

FTransform USkeletalMeshComponent::GetBoneLocalTransform(int32 BoneIndex) const
{
	if (BoneIndex < 0 || BoneIndex >= BoneSpaceTransforms.Num()) return FTransform();
	return BoneSpaceTransforms[BoneIndex];
}

void USkeletalMeshComponent::SetBoneLocalTransform(int32 BoneIndex, const FTransform& Local)
{
	if (BoneIndex < 0 || BoneIndex >= BoneSpaceTransforms.Num()) return;
	BoneSpaceTransforms[BoneIndex] = Local;
	MarkBoneTransformsDirty();
}

void USkeletalMeshComponent::SetBoneSpaceTransforms(const FCompactPose& Pose)
{
	if (Pose.GetNumBones() != GetBones().Num())
	{
		KRS_LOG(Warning, "SetBoneSpaceTransforms: pose has {} bones, mesh has {}.", Pose.GetNumBones(), GetBones().Num());
		return;
	}
#if defined(_DEBUG)
	for (int32 i = 0; i < Pose.GetNumBones(); ++i)
	{
		const FQuat& Q = Pose[i].Rotation;
		const float LenSq = Q.X * Q.X + Q.Y * Q.Y + Q.Z * Q.Z + Q.W * Q.W;
		if (std::abs(LenSq - 1.0f) > 1e-3f)
			KRS_LOG(Warning, "Pose bone {} rotation not normalized (|q|^2 = {})", i, LenSq);
	}
#endif
	BoneSpaceTransforms = Pose.GetBones();
	MarkBoneTransformsDirty();
}

FMatrix USkeletalMeshComponent::GetBoneComponentMatrix(int32 BoneIndex) const
{
	if (BoneIndex < 0 || BoneIndex >= ComponentSpaceTransforms.Num()) return FMatrix::Identity;
	return ComponentSpaceTransforms[BoneIndex];
}

FMatrix USkeletalMeshComponent::GetBoneWorldMatrix(int32 BoneIndex) const
{
	return GetBoneComponentMatrix(BoneIndex) * GetWorldMatrix();
}

namespace
{
	void ExtractSocketBasis(const FMatrix& Matrix, FQuat* OutRotation, FVector* OutScale)
	{
		FVector AxisX(Matrix[0][0], Matrix[0][1], Matrix[0][2]);
		FVector AxisY(Matrix[1][0], Matrix[1][1], Matrix[1][2]);
		FVector AxisZ(Matrix[2][0], Matrix[2][1], Matrix[2][2]);

		const float ScaleX = AxisX.Size();
		const float ScaleY = AxisY.Size();
		const float ScaleZ = AxisZ.Size();
		if (OutScale)
		{
			*OutScale = FVector(ScaleX, ScaleY, ScaleZ);
		}
		if (!OutRotation)
		{
			return;
		}
		if (ScaleX <= 1e-6f || ScaleY <= 1e-6f || ScaleZ <= 1e-6f)
		{
			*OutRotation = FQuat::Identity();
			return;
		}

		AxisX /= ScaleX;
		AxisY /= ScaleY;
		AxisZ /= ScaleZ;
		if (FVector::Dot(FVector::Cross(AxisX, AxisY), AxisZ) < 0.0f)
		{
			AxisZ = -AxisZ;
		}
		FMatrix RotMatrix = FMatrix::Identity;
		RotMatrix[0][0] = AxisX.X; RotMatrix[0][1] = AxisX.Y; RotMatrix[0][2] = AxisX.Z;
		RotMatrix[1][0] = AxisY.X; RotMatrix[1][1] = AxisY.Y; RotMatrix[1][2] = AxisY.Z;
		RotMatrix[2][0] = AxisZ.X; RotMatrix[2][1] = AxisZ.Y; RotMatrix[2][2] = AxisZ.Z;
		*OutRotation = FQuat(MatrixToRotator(RotMatrix));
	}
}

bool USkeletalMeshComponent::DoesSocketExist(const FString& InSocketName) const
{
	const USkeletalMesh* Mesh = GetSkeletalMesh();
	if (!Mesh || InSocketName.empty())
		return false;
	if (Mesh->HasSocket(InSocketName))
		return true;
	return Mesh->FindBoneIndex(InSocketName) >= 0;
}

FMatrix USkeletalMeshComponent::GetSocketWorldMatrix(const FString& InSocketName) const
{
	const FMatrix ComponentWorld = GetWorldMatrix();
	const USkeletalMesh* Mesh = GetSkeletalMesh();
	if (!Mesh || InSocketName.empty())
		return ComponentWorld;

	FMatrix LocalToBone = FMatrix::Identity;
	int32 BoneIndex = -1;
	if (const FSkeletalMeshSocket* Socket = Mesh->FindSocket(InSocketName))
	{
		BoneIndex = Mesh->FindBoneIndex(Socket->BoneName);
		if (BoneIndex >= 0)
			LocalToBone = Socket->GetSocketLocalMatrix();
	}
	if (BoneIndex < 0)
	{
		BoneIndex = Mesh->FindBoneIndex(InSocketName);
	}
	if (BoneIndex < 0)
		return ComponentWorld;

	return (LocalToBone * GetBoneComponentMatrix(BoneIndex)) * ComponentWorld;
}

FQuat USkeletalMeshComponent::GetSocketWorldRotationQuat(const FString& InSocketName) const
{
	if (InSocketName.empty())
		return FQuat(GetWorldRotation());
	FQuat OutRotation = FQuat::Identity();
	ExtractSocketBasis(GetSocketWorldMatrix(InSocketName), &OutRotation, nullptr);
	return OutRotation;
}

FVector USkeletalMeshComponent::GetSocketWorldScale3D(const FString& InSocketName) const
{
	if (InSocketName.empty())
		return GetWorldScale3D();
	FVector OutScale(1.0f, 1.0f, 1.0f);
	ExtractSocketBasis(GetSocketWorldMatrix(InSocketName), nullptr, &OutScale);
	return OutScale;
}

FTransform USkeletalMeshComponent::GetSocketTransform(const FString& InSocketName) const
{
	const FMatrix WorldMat = GetSocketWorldMatrix(InSocketName);
	FQuat Rot = FQuat::Identity();
	FVector Scale(1.0f, 1.0f, 1.0f);
	ExtractSocketBasis(WorldMat, &Rot, &Scale);
	return FTransform(Rot, FVector(WorldMat[3][0], WorldMat[3][1], WorldMat[3][2]), Scale);
}

void USkeletalMeshComponent::GetSocketNames(TArray<FString>& OutSocketNames) const
{
	OutSocketNames.Reset();
	const USkeletalMesh* Mesh = GetSkeletalMesh();
	if (!Mesh)
		return;
	for (const FSkeletalMeshSocket& Socket : Mesh->GetSockets())
	{
		OutSocketNames.Add(Socket.SocketName);
	}
}

void USkeletalMeshComponent::ResetPose()
{
	OnSkeletalMeshChanged();
	MarkBoneTransformsDirty();
}

void USkeletalMeshComponent::RefreshBoneTransformsNow()
{
	UpdateBoneTransformsNow();
}

void USkeletalMeshComponent::RefreshBoneTransforms()
{
	const TArray<FBoneInfo>& Bones = GetBones();
	if (BoneSpaceTransforms.Num() != Bones.Num())
		OnSkeletalMeshChanged();

	ComponentSpaceTransforms.SetNum(Bones.Num());
	for (int i = 0; i < Bones.Num(); ++i)
	{
		const FMatrix Local = BoneSpaceTransforms[i].GetLocalMatrix();
		const int32 Parent = Bones[i].ParentIndex;
		ComponentSpaceTransforms[i] = Parent < 0 ? Local : Local * ComponentSpaceTransforms[Parent];
	}
}

void USkeletalMeshComponent::Serialize(FStructuredArchive::FRecord Record)
{
    FArchive& Ar = Record.GetUnderlyingArchive();
    UAnimInstance* SavedInstance = AnimInstance;

    if (Ar.IsSaving() && (!AnimBlueprintPath.empty() || ManagedAnimGraph))
    {
        AnimInstance = nullptr;
    }

    Super::Serialize(Record);

    if (Ar.IsSaving())
    {
        AnimInstance = SavedInstance;
    }

	int32 Num = Ar.IsSaving() ? BoneSpaceTransforms.Num() : 0;
	FStructuredArchive::FArray PoseArray = Record.EnterArray("BonePose", Num);

	for (int32 I = 0; I < Num; ++I)
	{
		FStructuredArchive::FRecord BoneRecord = PoseArray.EnterElement().EnterRecord();
		FString Name = Ar.IsSaving() ? GetBones()[I].Name : FString();
		FTransform Local = Ar.IsSaving() ? BoneSpaceTransforms[I] : FTransform();
		BoneRecord << SA_VALUE("Name", Name) << SA_VALUE("Local", Local);

		if (Ar.IsLoading())
		{
			// 메시의 본 순서가 바뀌어도 맞도록 이름으로 찾는다
			const int32 Index = FindBoneIndex(Name);
			if (Index >= 0)
				BoneSpaceTransforms[Index] = Local;
		}
	}

	SerializeSingleAnimation(Record);

    if (Ar.IsLoading())
    {
        if (!AnimBlueprintPath.empty())
        {
            ReloadAnimBlueprint();
        }

        RefreshBoneTransformsNow();
    }
}

void USkeletalMeshComponent::SerializeSingleAnimation(FStructuredArchive::FRecord& Record)
{
	// 디테일 패널에서 고른 단일 애니메이션(SingleNodeInstance의 에셋)은 AnimInstance 객체가 아니라
	// 에셋 경로로 저장한다. 없으면 필드를 쓰지 않으므로 예전 씬도 그대로 읽힌다.
	FArchive& Ar = Record.GetUnderlyingArchive();
	UAnimSingleNodeInstance* Instance = GetSingleNodeInstance();
	UAnimationAsset* Asset = Instance ? Instance->GetCurrentAsset() : nullptr;
	const bool bHasAnimation = Ar.IsSaving() && Asset && !Asset->GetAssetPath().empty();

	TOptional<FStructuredArchive::FSlot> AnimSlot = Record.TryEnterField("SingleAnimation", bHasAnimation);
	if (!AnimSlot.IsSet())
		return;

	FStructuredArchive::FRecord AnimRecord = AnimSlot.GetValue().EnterRecord();
	FString Path = Ar.IsSaving() ? Asset->GetAssetPath() : FString();
	bool bLoop = Ar.IsSaving() ? Instance->IsLooping() : true;
	float Rate = Ar.IsSaving() ? Instance->GetPlayRate() : 1.0f;
	AnimRecord << SA_VALUE("Asset", Path) << SA_VALUE("Looping", bLoop) << SA_VALUE("PlayRate", Rate);

	if (!Ar.IsLoading())
		return;

	// AssetManager 캐시로 불러온다 (디테일 패널이 고를 때와 같은 객체)
	UAnimationAsset* Loaded = BlendSpaceFile::IsBlendSpacePath(Path)
		? static_cast<UAnimationAsset*>(UAssetManager::LoadBlendSpace(Path))
		: static_cast<UAnimationAsset*>(UAssetManager::LoadAnimSequence(Path));
	if (!Loaded)
	{
		KRS_LOG(Warning, "SkeletalMeshComponent: single animation not found: {}", Path);
		return;
	}

	// 재생 시작은 BeginPlay가 한다 (에디터 월드에서는 첫 프레임 포즈만 보인다)
	SetAnimation(Loaded);
	if (UAnimSingleNodeInstance* NewInstance = GetSingleNodeInstance())
	{
		NewInstance->SetLooping(bLoop);
		NewInstance->SetPlayRate(Rate);
	}
}

void USkeletalMeshComponent::OnSkeletalMeshChanged()
{
    bBlueprintReloadRequested = !AnimBlueprintPath.empty();
	BoneSpaceTransforms.Reset();
	for (const FBoneInfo& Bone : GetBones())
		BoneSpaceTransforms.Add(Bone.LocalBindPose);
}

void USkeletalMeshComponent::InitConstraints(const UPhysicsAsset& Asset)
{
	const auto& Bones = GetBones();

	TArray<physx::PxTransform> BindPoses;
	BuildBindPoses(Bones, GetWorldScale3D(), BindPoses);

	int32 NumFailed = 0;
	for (const auto& Constraint : Asset.Constraints)
	{
		const int32 ChildBone = FindBoneIndex(Constraint.ChildBone);
		const int32 ParentBone = FindBoneIndex(Constraint.ParentBone);
		FBodyInstance* ChildBody = ChildBone >= 0 ? BoneToBody[ChildBone] : nullptr;
		FBodyInstance* ParentBody = ParentBone >= 0 ? BoneToBody[ParentBone] : nullptr;
		if (!ChildBody || !ParentBody)
		{
			NumFailed++;
			continue;
		}

		const physx::PxQuat TwistAlign = physx::PxShortestRotation(physx::PxVec3(1, 0, 0), K2PVector(FPhysicsBoneUtils::FindBoneAxis(Bones, ChildBone)));
		const physx::PxQuat Offset = K2PQuat(Constraint.FrameOffset.Quaternion().Normalized());
		const physx::PxTransform JointBind(BindPoses[ChildBone].p, (BindPoses[ChildBone].q * TwistAlign * Offset).getNormalized());

		const physx::PxTransform Frame0 = BindPoses[ParentBone].getInverse() * JointBind;
		const physx::PxTransform Frame1 = BindPoses[ChildBone].getInverse() * JointBind;

		TUniquePtr<FConstraintInstance> ConstraintInstance = MakeUnique<FConstraintInstance>();
		ConstraintInstance->LinearX = EConstraintMotion::Locked;
		ConstraintInstance->LinearY = EConstraintMotion::Locked;
		ConstraintInstance->LinearZ = EConstraintMotion::Locked;
		ConstraintInstance->Swing1Motion = Constraint.Swing1Motion;
		ConstraintInstance->Swing2Motion = Constraint.Swing2Motion;
		ConstraintInstance->TwistMotion = Constraint.TwistMotion;
		ConstraintInstance->Swing1LimitAngle = Constraint.Swing1LimitDeg;
		ConstraintInstance->Swing2LimitAngle = Constraint.Swing2LimitDeg;
		ConstraintInstance->TwistLimitLowerAngle = Constraint.TwistMinDeg;
		ConstraintInstance->TwistLimitUpperAngle = Constraint.TwistMaxDeg;

		ConstraintInstance->InitConstraint(ParentBody, ChildBody, P2KTransform(Frame0), P2KTransform(Frame1));

		if (ConstraintInstance->IsValidConstraint())
		{
			Constraints.Add(std::move(ConstraintInstance));
		}
		else
		{
			NumFailed++;
		}
	}

	KRS_LOG(Info, "[Ragdoll] constraints created: {}, failed {}", Constraints.Num(), NumFailed);
}

UAnimSingleNodeInstance* USkeletalMeshComponent::GetSingleNodeInstance() const
{
	return Cast<UAnimSingleNodeInstance>(AnimInstance);
}

void USkeletalMeshComponent::SetAnimation(UAnimationAsset* NewAnimToPlay)
{
	if (NewAnimToPlay && !Cast<UAnimSequence>(NewAnimToPlay) && !Cast<UBlendSpace1D>(NewAnimToPlay))
	{
		KRS_LOG(Warning, "SetAnimation: unsupported animation asset type");
		return;
	}
    AnimBlueprintPath.clear();
    AnimBlueprintError.clear();
    bBlueprintReloadRequested = false;
	auto* SingleNodeInstance = Cast<UAnimSingleNodeInstance>(AnimInstance);

	if (!SingleNodeInstance)
	{
		SingleNodeInstance = NewObject<UAnimSingleNodeInstance>(this);
		SetAnimInstance(SingleNodeInstance);
	}

	SingleNodeInstance->SetAnimationAsset(NewAnimToPlay);
	SingleNodeInstance->SetBlendSpaceInput(BlendSpaceInput);
	SingleNodeInstance->SetBlendSpace2DInput(BlendSpaceInput, BlendSpaceInputY);

	RefreshAnimationPose();
}

void USkeletalMeshComponent::Play(bool bLooping)
{
	if (auto* Instance = GetSingleNodeInstance())
	{
		Instance->SetLooping(bLooping);
		Instance->SetPlaying(true);
	}
}

void USkeletalMeshComponent::Stop()
{
	if (auto* Instance = GetSingleNodeInstance())
	{
		Instance->SetPlaying(false);
	}
}

bool USkeletalMeshComponent::IsPlaying() const
{
	const auto* Instance = GetSingleNodeInstance();
	return Instance && Instance->IsPlaying();
}

void USkeletalMeshComponent::PlayAnimation(UAnimationAsset* NewAnimToPlay, bool bLooping)
{
	if (NewAnimToPlay && !Cast<UAnimSequence>(NewAnimToPlay) && !Cast<UBlendSpace1D>(NewAnimToPlay) && !Cast<UBlendSpace2D>(NewAnimToPlay))
	{
		return;
	}

	SetAnimation(NewAnimToPlay);
	Play(bLooping);
}

void USkeletalMeshComponent::RefreshAnimationPose()
{
    if (!AnimInstance)
    {
        ResetPose();
        RefreshBoneTransformsNow();
        return;
    }

	if (UAnimSingleNodeInstance* Instance = GetSingleNodeInstance())
	{
		Instance->SetBlendSpaceInput(BlendSpaceInput);
		Instance->SetBlendSpace2DInput(BlendSpaceInput, BlendSpaceInputY);
	}

	FPoseContext Pose(AnimInstance, GetBones());
	AnimInstance->EvaluateAnimation(Pose);
	SetBoneSpaceTransforms(Pose.Pose);
	RefreshBoneTransformsNow();
}


FTransform USkeletalMeshComponent::ConsumeRootMotion()
{
	const FTransform Result = PendingRootMotion;
	PendingRootMotion = FTransform();
	bHasPendingRootMotion = false;
	return Result;
}

FTransform USkeletalMeshComponent::ConvertRootMotionToWorld(const FTransform& ParentSpaceDelta) const
{
	// delta는 root 본의 부모 공간 값이다. UAL은 Armature → root 구조라 부모(Armature)를 거쳐야 컴포넌트 공간이 된다.
	const TArray<FBoneInfo>& Bones = GetBones();
	const int32 RootIndex = GetRootMotionBoneIndex();
	const int32 ParentIndex = RootIndex >= 0 ? Bones[RootIndex].ParentIndex : -1;

	// 이동: 부모(바인드 포즈) → 컴포넌트 → 월드 행렬로 방향·스케일만 적용 (Armature 스케일, 메시 0.01, yaw −180 모두 여기서)
	const FMatrix ComponentToWorld = GetWorldMatrix();
	const FMatrix ParentToWorld = ParentIndex >= 0
		? Bones[ParentIndex].InverseBindPose.Inverse() * ComponentToWorld
		: ComponentToWorld;

	FTransform Result;
	Result.Location = ParentToWorld.TransformVector(ParentSpaceDelta.Location);

	// 회전: 부모 공간 회전을 월드 공간 회전으로 바꾼다 (P × Δ × P⁻¹, P = 컴포넌트 월드 회전 × 부모 본 회전)
	FQuat ParentRotation = FQuat(GetWorldRotation());
	FQuat BoneChainRotation;
	for (int32 Index = ParentIndex; Index >= 0; Index = Bones[Index].ParentIndex)
	{
		BoneChainRotation = Bones[Index].LocalBindPose.Rotation * BoneChainRotation;
	}
	ParentRotation = (ParentRotation * BoneChainRotation).Normalized();
	Result.Rotation = (ParentRotation * ParentSpaceDelta.Rotation * ParentRotation.Inverse()).Normalized();
	return Result;
}

int32 USkeletalMeshComponent::GetRootMotionBoneIndex() const
{
	const int32 RootIndex = FindBoneIndex("root");
	if (RootIndex >= 0)
	{
		return RootIndex;
	}
	return GetBones().IsEmpty() ? -1 : 0;
}

void USkeletalMeshComponent::DrawRootDebug(FLineBatcher* LineBatcher) const
{
	const int32 RootIndex = GetRootMotionBoneIndex();
	if (!LineBatcher || RootIndex < 0 || RootIndex >= ComponentSpaceTransforms.Num())
	{
		return;
	}

	// root 본 위치에 구체
	const FVector Root = GetBoneWorldMatrix(RootIndex).TransformPosition(FVector::ZeroVector);
	const FVector4 Color(1.0f, 0.3f, 0.8f, 1.0f);
	LineBatcher->AddSphere(Root, 0.08f, Color);

	// 액터 위치를 root 높이로 내린 점 → root. 선 길이 = root가 액터(캡슐)에서 벗어난 거리
	if (const AActor* Owner = GetOwner())
	{
		const FVector ActorLocation = Owner->GetActorLocation();
		const FVector Base(ActorLocation.X, ActorLocation.Y, Root.Z);
		LineBatcher->AddLine(Base, Root, Color);
		LineBatcher->AddSphere(Base, 0.03f, Color);
	}
}
bool GShowSkeletalRootDebug = false;

void DrawSkeletalRootDebug(UWorld* World, FLineBatcher* LineBatcher)
{
	if (!World || !LineBatcher) return;
	for (TObjectIterator<USkeletalMeshComponent> It; It; ++It)
	{
		const USkeletalMeshComponent* Mesh = *It;
		if (!Mesh || Mesh->GetWorld() != World || !Mesh->IsRegistered()) continue;
		Mesh->DrawRootDebug(LineBatcher);
	}
}

void DrawPhysicsAssetBodies(UWorld* World, FLineBatcher* LineBatcher)
{
	if (!World || !LineBatcher) return;
	for (TObjectIterator<USkeletalMeshComponent> It; It; ++It)
	{
		const USkeletalMeshComponent* Mesh = *It;
		if (!Mesh || Mesh->GetWorld() != World || !Mesh->IsRegistered()) continue;
		Mesh->DrawPhysicsAssetDebug(LineBatcher);
	}
}

void FSkeletalMeshComponentEndPhysicsTickFunction::ExecuteTick(float DeltaTime)
{
	Target->EndPhysicsTickComponent(DeltaTime);
}

void USkeletalMeshComponent::DrawPhysicsAssetDebug(FLineBatcher* LineBatcher) const
{
	const USkeletalMesh* Mesh = GetSkeletalMesh();
	const UPhysicsAsset* PhysicsAsset = GetPhysicsAsset();
	if (!LineBatcher || !PhysicsAsset)
	{
		return;
	}

	const FVector4 ShapeColor(0.2f, 1.0f, 0.4f, 1.0f);
	const FVector4 OffsetColor(1.0f, 0.9f, 0.2f, 1.0f);

	for (const UBodySetup* Body : PhysicsAsset->BodySetups)
	{
		if (!Body) continue;
		const int32 BoneIndex = Mesh->FindBoneIndex(Body->BoneName);
		if (BoneIndex < 0 || BoneIndex >= ComponentSpaceTransforms.Num()) continue;

		const FMatrix BoneToWorld = GetBoneWorldMatrix(BoneIndex);
		const FVector BoneOrigin = BoneToWorld.TransformPosition(FVector::ZeroVector);
		const FAggregateGeom& Geom = Body->AggGeom;

		auto ShapeToWorld = [&](const FShapeElem& Elem)
		{
			const FMatrix M = FTransform(Elem.Rotation, Elem.Center).GetLocalMatrix() * BoneToWorld;
			LineBatcher->AddLine(BoneOrigin, M.TransformPosition(FVector::ZeroVector), OffsetColor);
			return M;
		};

		for (const FSphereElem& Elem : Geom.SphereElems)
		{
			DrawLocalSphere(LineBatcher, ShapeToWorld(Elem), Elem.Radius, ShapeColor);
		}
		for (const FBoxElem& Elem : Geom.BoxElems)
		{
			const FVector E(0.5f * Elem.X, 0.5f * Elem.Y, 0.5f * Elem.Z);
			LineBatcher->AddOrientedBox(FBox{ E * -1.0f, E }, ShapeToWorld(Elem), ShapeColor);
		}
		for (const FCapsuleElem& Elem : Geom.CapsuleElems)
		{
			DrawLocalCapsule(LineBatcher, ShapeToWorld(Elem), Elem.Radius, Elem.Length, ShapeColor);
		}
	}
}

USkeletalMeshComponent::USkeletalMeshComponent()
{
	EndPhysicsTickFunction.Target = this;
	EndPhysicsTickFunction.bCanEverTick = true;
	EndPhysicsTickFunction.bStartWithTickEnabled = false;
	EndPhysicsTickFunction.TickGroup = ETickingGroup::PostPhysics;
}

USkeletalMeshComponent::~USkeletalMeshComponent()
{
    if (ManagedAnimGraph)
    {
        if (AnimInstance == ManagedAnimGraph.get())
        {
            SetAnimInstance(nullptr);
        }
        else
        {
            ManagedAnimGraph->UninitializeAnimation();
        }
    }
}

const FString& USkeletalMeshComponent::GetAnimBlueprintPath() const
{
    return AnimBlueprintPath;
}

const FString& USkeletalMeshComponent::GetAnimBlueprintError() const
{
    return AnimBlueprintError;
}

bool USkeletalMeshComponent::SetAnimBlueprint(const FString& Path)
{
    if (bUpdatingAnimation)
    {
        AnimBlueprintError = "Load or reload Animation Blueprints from actor Tick, outside animation callbacks.";
        return false;
    }

    if (Path.empty())
    {
        const bool bHadBlueprint = !AnimBlueprintPath.empty();
        AnimBlueprintPath.clear();
        AnimBlueprintError.clear();
        bBlueprintReloadRequested = false;

        if (ManagedAnimGraph && bHadBlueprint)
        {
            SetAnimInstance(nullptr);
            ResetPose();
            RefreshBoneTransformsNow();
        }

        return true;
    }

    AnimBlueprintError.clear();
    const FString Key = FAssetUtils::GetAssetKey(Path);
    const FString FilePath = FAssetUtils::ResolveAssetFilePath(Key);
    FAnimBlueprintData Data;

    if (!AnimBlueprintFile::IsAnimBlueprintPath(Key) || !AnimBlueprintFile::Read(FPaths::FromUtf8(FilePath.empty() ? Key : FilePath), Data, AnimBlueprintError))
    {
        if (AnimBlueprintError.empty())
        {
            AnimBlueprintError = "Use an .animblueprint asset.";
        }

        return false;
    }

    TUniquePtr<UAnimGraphInstance> Instance;
    AnimBlueprintCompiler::FNodeMap NodeIndices;

    if (!AnimBlueprintCompiler::Build(Data, *this, Instance, NodeIndices, AnimBlueprintError))
    {
        return false;
    }

    SetAnimInstance(Instance.get());
    ManagedAnimGraph = std::move(Instance);
    AnimBlueprintPath = Key;
    bBlueprintReloadRequested = false;
    AnimBlueprintError.clear();
    RefreshAnimationPose();
    return true;
}

bool USkeletalMeshComponent::ReloadAnimBlueprint()
{
    const FString Path = AnimBlueprintPath;
    bBlueprintReloadRequested = false;

    if (Path.empty())
    {
        return false;
    }

    if (!SetAnimBlueprint(Path))
    {
        KRS_LOG(Warning, "AnimBlueprint load failed: {} ({})", Path, AnimBlueprintError);
        return false;
    }

    return true;
}

UAnimGraphInstance* USkeletalMeshComponent::CreateAnimGraph()
{
    if (bUpdatingAnimation)
    {
        AnimBlueprintError = "Create animation graphs outside animation callbacks.";
        return nullptr;
    }

    if (!AnimBlueprintPath.empty())
    {
        AnimBlueprintError = "An Animation Blueprint is assigned. Use GetAnimGraph to update its variables.";
        return nullptr;
    }

    AnimBlueprintError.clear();

    if (UAnimGraphInstance* Existing = Cast<UAnimGraphInstance>(AnimInstance))
    {
        Existing->ResetGraph();
        return Existing;
    }

    TUniquePtr<UAnimGraphInstance> Instance(NewObject<UAnimGraphInstance>(this));
    SetAnimInstance(Instance.get());
    ManagedAnimGraph = std::move(Instance);
    return ManagedAnimGraph.get();
}

void USkeletalMeshComponent::StartPreviewDissolve(float InDuration, const FVector4& InEdgeColor)
{
	USkeletalMesh* Mesh = GetSkeletalMesh();
	if (!Mesh)
	{
		return;
	}
	if (PreviewDissolveSavedMaterials.IsEmpty())
	{
		const int32 SlotCount = GetNumMaterials();
		PreviewDissolveSavedMaterials.SetNum(SlotCount);
		for (int32 SlotIndex = 0; SlotIndex < SlotCount; ++SlotIndex)
		{
			UMaterialInterface* Original = GetMaterial(SlotIndex);
			PreviewDissolveSavedMaterials[SlotIndex] = Original;
			UMaterial* BaseMat = Original ? Original->GetBaseMaterial() : nullptr;
			UMaterial* DissolveParent = UMaterial::GetOrCreateDissolveParent(BaseMat);
			UMaterialInstanceDynamic* DynMat = DissolveParent ? UMaterial::CreateDynamicInstance(DissolveParent) : nullptr;
			if (!DynMat)
			{
				continue;
			}
			SetMaterial(SlotIndex, DynMat);
			PreviewDissolveMaterials.Add(DynMat);
		}
	}

	PreviewDissolveDuration = (std::max)(0.05f, InDuration);
	PreviewDissolveElapsed = 0.0f;
	bPreviewDissolveActive = true;
	SetDissolveHidden(false);

	const FBox MeshBounds = CalcBounds();
	const float MinZ = MeshBounds.Min.Z;
	const float MaxZ = (std::max)(MeshBounds.Max.Z, MinZ + 0.1f);
	for (UMaterialInstanceDynamic* DynMat : PreviewDissolveMaterials)
	{
		if (!DynMat)
		{
			continue;
		}
		DynMat->SetScalarParameterValue(FName("DissolveAmount"), -0.05f);
		DynMat->SetScalarParameterValue(FName("DissolveMinZ"), MinZ);
		DynMat->SetScalarParameterValue(FName("DissolveMaxZ"), MaxZ);
		DynMat->SetVectorParameterValue(FName("DissolveEdgeColor"), InEdgeColor);
	}
	if (AActor* Owner = GetOwner())
	{
		for (UActorComponent* Comp : Owner->GetComponents())
		{
			if (UParticleSystemComponent* ParticleComp = Cast<UParticleSystemComponent>(Comp))
			{
				ParticleComp->SetFloatParameter("DissolveHeight", MaxZ);
			}
		}
	}
}

void USkeletalMeshComponent::UpdatePreviewDissolve(float DeltaTime)
{
	if (!bPreviewDissolveActive || !std::isfinite(DeltaTime) || DeltaTime <= 0.0f)
	{
		return;
	}

	PreviewDissolveElapsed += DeltaTime;
	const float Alpha = std::clamp(PreviewDissolveElapsed / (std::max)(0.05f, PreviewDissolveDuration), 0.0f, 1.0f);
	const float Amount = -0.05f + Alpha * 1.13f;
	const FBox MeshBounds = CalcBounds();
	const float MinZ = MeshBounds.Min.Z;
	const float MaxZ = (std::max)(MeshBounds.Max.Z, MinZ + 0.1f);
	for (UMaterialInstanceDynamic* DynMat : PreviewDissolveMaterials)
	{
		if (DynMat)
		{
			DynMat->SetScalarParameterValue(FName("DissolveAmount"), Amount);
			DynMat->SetScalarParameterValue(FName("DissolveMinZ"), MinZ);
			DynMat->SetScalarParameterValue(FName("DissolveMaxZ"), MaxZ);
		}
	}
	if (AActor* Owner = GetOwner())
	{
		const float ActiveNormZ = std::clamp(1.0f - (Amount - 0.125f) / 0.80f, 0.0f, 1.0f);
		const float DissolveWorldZ = MinZ + ActiveNormZ * (MaxZ - MinZ);
		for (UActorComponent* Comp : Owner->GetComponents())
		{
			if (UParticleSystemComponent* ParticleComp = Cast<UParticleSystemComponent>(Comp))
			{
				ParticleComp->SetFloatParameter("DissolveHeight", DissolveWorldZ);
			}
		}
	}
	if (Alpha >= 1.0f)
	{
		bPreviewDissolveActive = false;
		SetDissolveHidden(true);
	}
}

void USkeletalMeshComponent::ResetPreviewDissolve()
{
	if (!bPreviewDissolveActive && PreviewDissolveMaterials.IsEmpty() && PreviewDissolveSavedMaterials.IsEmpty())
	{
		return;
	}
	bPreviewDissolveActive = false;
	PreviewDissolveElapsed = 0.0f;
	for (int32 SlotIndex = 0; SlotIndex < PreviewDissolveSavedMaterials.Num(); ++SlotIndex)
	{
		SetMaterial(SlotIndex, PreviewDissolveSavedMaterials[SlotIndex]);
	}
	PreviewDissolveSavedMaterials.Reset();
	PreviewDissolveMaterials.Reset();
	SetDissolveHidden(false);
}

void USkeletalMeshComponent::SetDissolveHidden(bool bHidden)
{
	if (bDissolveHidden == bHidden)
	{
		return;
	}
	bDissolveHidden = bHidden;
	SetVisibility(!bHidden);
}

void USkeletalMeshComponent::OnCreatePhysicsState()
{
	Super::OnCreatePhysicsState();
	InitArticulated(GetWorld() ? GetWorld()->GetPhysicsScene() : nullptr);
}

void USkeletalMeshComponent::OnDestroyPhysicsState()
{
	TermArticulated();
	Super::OnDestroyPhysicsState();
}

bool USkeletalMeshComponent::IsSimulatingRagdoll() const
{
	// 부분 시뮬레이션은 메시 전체를 이동시키지 않고 선택한 본들의 포즈만 갱신한다.
	if (Bodies.IsEmpty() || bSimulatingBodiesBelow) return false;
	for (const auto& Body : Bodies)
		if (!Body->IsDynamic()) return false;
	return true;
}

void USkeletalMeshComponent::UpdateComponentToRootBody()
{
	FBodyInstance* RootBody = nullptr;
	for (auto* Body : BoneToBody)
	{
		if (Body && Body->IsDynamic())
		{
			RootBody = Body;
			break;
		}
	}
	if (!RootBody)
	{
		return;
	}

	const FVector Delta = RootBody->GetUnrealWorldTransform().Location - GetWorldLocation();

	AActor* Owner = GetOwner();
	USceneComponent* Target = (Owner && Owner->GetRootComponent()) ? Owner->GetRootComponent() : this;

	Target->SetWorldLocation(Target->GetWorldLocation() + Delta, false, ETeleportType::None);
}

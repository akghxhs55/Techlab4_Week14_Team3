#include "EnginePCH.h"
#include "Components/StockingComponent.h"
#include "Asset/AssetUtils.h"
#include "Core/Misc/Paths.h"
#include "Engine/StockingSceneProxy.h"
#include "Engine/World.h"
#include "Physics/ClothCore.h"
#include "Render/SkeletalMeshObject.h"
#include "Render/Material.h"
#include "Render/MaterialInstance.h"
#include "Render/Texture2D.h"
#include "Serialization/StructuredArchive.h"
#include "UObject/UObjectGlobals.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <functional>
#include <queue>
#include <unordered_map>

namespace
{
	uint64 MixStockingHash(uint64 Hash, uint64 Value)
	{
		Hash ^= Value;
		Hash *= 1099511628211ull;
		return Hash;
	}

	uint64 MakeStockingPositionKey(const FVector& Pos, float QuantScale)
	{
		const int64 QX = static_cast<int64>(std::llround(Pos.X * QuantScale));
		const int64 QY = static_cast<int64>(std::llround(Pos.Y * QuantScale));
		const int64 QZ = static_cast<int64>(std::llround(Pos.Z * QuantScale));
		uint64 Hash = 14695981039346656037ull;
		Hash = MixStockingHash(Hash, static_cast<uint64>(QX));
		Hash = MixStockingHash(Hash, static_cast<uint64>(QY));
		Hash = MixStockingHash(Hash, static_cast<uint64>(QZ));
		return Hash;
	}

	bool AreVectorsClose(const FVector& A, const FVector& B, float Tolerance)
	{
		return std::fabs(A.X - B.X) <= Tolerance &&
			std::fabs(A.Y - B.Y) <= Tolerance &&
			std::fabs(A.Z - B.Z) <= Tolerance;
	}

	bool AreQuatsClose(const FQuat& A, const FQuat& B, float Tolerance)
	{
		const float Dot = std::fabs(A.X * B.X + A.Y * B.Y + A.Z * B.Z + A.W * B.W);
		return (1.0f - std::min(Dot, 1.0f)) <= Tolerance;
	}

	bool IsInGameWorld(const UActorComponent* Component)
	{
		const UWorld* World = Component ? Component->GetWorld() : nullptr;
		return World && World->IsGameWorld();
	}

	bool AreMatricesClose(const FMatrix& A, const FMatrix& B, float Tolerance)
	{
		for (int32 r = 0; r < 4; ++r)
		{
			for (int32 c = 0; c < 4; ++c)
			{
				if (std::fabs(A.M[r][c] - B.M[r][c]) > Tolerance)
				{
					return false;
				}
			}
		}
		return true;
	}
}

UStockingComponent::UStockingComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	// 원본 애니메이션과 본 갱신 이후 스타킹 스키닝 실행 보장
	PrimaryComponentTick.TickGroup = ETickingGroup::PostUpdateWork;
	bCastShadow = true;
	bReceiveShadow = true;
}

UStockingComponent::~UStockingComponent()
{
	UnbindSourceImpactListener();
	DestroyNvClothResources();
}

void UStockingComponent::TickComponent(float DeltaTime)
{
	if (DeltaTime > 0.0f && std::isfinite(DeltaTime))
	{
		PendingClothDeltaTime = std::clamp(DeltaTime, 1.0f / 120.0f, 1.0f / 30.0f);
		TearMaskUploadCooldown = std::max(0.0f, TearMaskUploadCooldown - DeltaTime);
		PullTearCooldown = std::max(0.0f, PullTearCooldown - DeltaTime);
		ImpactTearCooldownTimer = std::max(0.0f, ImpactTearCooldownTimer - DeltaTime);
	}
	bClothSteppedThisFrame = false;

	if (!SourceSkeletalMeshComponent)
	{
		if (USkeletalMeshComponent* ParentSkel = Cast<USkeletalMeshComponent>(GetAttachParent()))
		{
			SourceSkeletalMeshComponent = ParentSkel;
		}
	}
	if (BoundImpactSource != SourceSkeletalMeshComponent)
	{
		BindSourceImpactListener();
	}

	if (bTearMaskDirty && TearMaskUploadCooldown <= 0.0f)
	{
		RebuildTearMaskTexture();
	}

	if (IsInGameWorld(this))
	{
		if (HasActiveDeformation())
		{
			ResetPullImmediately();
		}
	}
	else
	{
		StepPullState(DeltaTime);
	}
	BindDeformationCallback();

	if (SourceSkeletalMeshComponent)
	{
		// 원본 본 변환이 아직 비어 있으면 먼저 갱신
		if (SourceSkeletalMeshComponent->GetComponentSpaceTransform().IsEmpty())
		{
			SourceSkeletalMeshComponent->UpdateBoneTransformsNow();
		}

		if (bSyncTransformWithSource)
		{
			const FMatrix SourceWorld = SourceSkeletalMeshComponent->GetWorldMatrix();
			const FMatrix SelfWorld = GetWorldMatrix();
			if (!AreMatricesClose(SourceWorld, SelfWorld, 1e-4f))
			{
				SetWorldTransform(SourceWorld, false);
			}
		}

		const TArray<FMatrix>& SourceTransforms = SourceSkeletalMeshComponent->GetComponentSpaceTransform();
		FSkeletalMeshObject* MeshObject = GetMeshObject();
		if (HasPoseOrPullChanged(SourceTransforms, MeshObject))
		{
			MarkBoneTransformsDirty();
			RecordSyncedPoseState(SourceTransforms, MeshObject);
		}
	}

	Super::TickComponent(DeltaTime);

	const float NeededExpansion = SurfaceOffset + MaxAppliedDisplacement + BoundsPadding;
	const float ExpandStep = 0.005f * ReferenceScale;
	const float ShrinkStep = 0.04f * ReferenceScale;
	if (NeededExpansion > CachedBoundsExpansion + ExpandStep ||
		(NeededExpansion < CachedBoundsExpansion - ShrinkStep && !bIsPulling))
	{
		CachedBoundsExpansion = NeededExpansion;
		OnTransformDirty(ETeleportType::None);
	}
}

void UStockingComponent::Serialize(FStructuredArchive::FRecord Record)
{
	Super::Serialize(Record);

	FArchive& Ar = Record.GetUnderlyingArchive();
	const bool bDuplicateForPIE = Ar.HasAnyPortFlags(EPropertyPortFlags::PPF_DuplicateForPIE);
	FString RegionFilePath = (!Ar.IsLoading() && StockingRegion) ? StockingRegion->GetRegionFilePath() : FString();
	if (!bDuplicateForPIE)
	{
		const bool bHasRegionPath = !RegionFilePath.empty();
		if (TOptional<FStructuredArchive::FSlot> RegionSlot = Record.TryEnterField("StockingRegionPath", bHasRegionPath))
		{
			RegionSlot.GetValue() << RegionFilePath;
		}
	}

	if (Ar.IsLoading())
	{
		if (!StockingRegion && !RegionFilePath.empty())
		{
			FString ResolvedPath = RegionFilePath;
			if (!std::filesystem::exists(FPaths::FromUtf8(ResolvedPath)))
			{
				const FString AssetFile = FAssetUtils::ResolveAssetFilePath(RegionFilePath);
				if (!AssetFile.empty())
				{
					ResolvedPath = AssetFile;
				}
			}
			if (std::filesystem::exists(FPaths::FromUtf8(ResolvedPath)))
			{
				UStockingRegion* LoadedRegion = NewObject<UStockingRegion>(this);
				FString LoadError;
				if (LoadedRegion && LoadedRegion->LoadFromFile(ResolvedPath, LoadError))
				{
					StockingRegion = LoadedRegion;
				}
			}
		}

		// PIE 복제는 하위 객체 직렬화가 모두 끝난 PostDuplicate에서 후처리한다
		if (!bDuplicateForPIE)
		{
			FinalizeStockingSetupAfterLoad();
		}
	}
}

void UStockingComponent::PostDuplicate(bool bDuplicateForPIE)
{
	Super::PostDuplicate(bDuplicateForPIE);
	FinalizeStockingSetupAfterLoad();
}

void UStockingComponent::FinalizeStockingSetupAfterLoad()
{
	if (!SourceSkeletalMeshComponent)
	{
		if (USkeletalMeshComponent* ParentSkel = Cast<USkeletalMeshComponent>(GetAttachParent()))
		{
			SourceSkeletalMeshComponent = ParentSkel;
		}
		else if (AActor* Owner = GetOwner())
		{
			for (UActorComponent* Comp : Owner->GetComponents())
			{
				if (USkeletalMeshComponent* SkelComp = Cast<USkeletalMeshComponent>(Comp))
				{
					if (static_cast<const UActorComponent*>(SkelComp) != this)
					{
						SourceSkeletalMeshComponent = SkelComp;
						break;
					}
				}
			}
		}
	}

	UMaterialInterface* SlotMat = GetOverrideMaterial(0);
	if (SlotMat && SlotMat->GetBaseMaterial())
	{
		if (!StockingMaterial)
		{
			if (UMaterialInstanceDynamic* SlotDyn = Cast<UMaterialInstanceDynamic>(SlotMat))
			{
				DefaultStockingMaterial = SlotDyn;
			}
		}
	}
	else
	{
		if (!StockingMaterial && StockingRegion && StockingRegion->bHasSavedMaterialParams)
		{
			if (UMaterialInstanceDynamic* DefaultMat = GetDefaultStockingMaterialInstance())
			{
				FStockingMaterialLibrary::ApplyParameters(DefaultMat, StockingRegion->SavedMaterialParams);
			}
		}
		SetMaterial(0, GetEffectiveStockingMaterial());
	}

	USkeletalMesh* ExistingMesh = GetSkeletalMesh();
	const bool bHasValidMesh = ExistingMesh && !ExistingMesh->GetRenderData().LODRenderData.IsEmpty();
	if (StockingRegion && SourceSkeletalMeshComponent && !bHasValidMesh && SourceSkeletalMeshComponent->GetSkeletalMesh())
	{
		FString Error;
		RebuildStockingMesh(Error);
	}
	else if (bHasValidMesh)
	{
		if (WeldedVertexCount == 0)
		{
			BuildRestAdjacency(*ExistingMesh);
			BuildCoveredBoneMask(*ExistingMesh);
		}
		BindSourceImpactListener();
	}

	EnsureTearMaskTexture();
	SyncTearMaskBinding();
	bHasSyncedPoseState = false;
	BindDeformationCallback();
}

FPrimitiveSceneProxy* UStockingComponent::CreateSceneProxy()
{
	return new FStockingSceneProxy(this);
}

FBox UStockingComponent::CalcLocalBounds() const
{
	FBox BaseBounds = Super::CalcLocalBounds();
	const float Expand = std::max(0.0f, SurfaceOffset + MaxAppliedDisplacement + BoundsPadding);
	const FVector Margin(Expand, Expand, Expand);
	BaseBounds.Min -= Margin;
	BaseBounds.Max += Margin;
	return BaseBounds;
}

void UStockingComponent::PostEditChangeProperty(const FProperty& Property)
{
	Super::PostEditChangeProperty(Property);

	const FString& Name = Property.Name;
	if (Name == "SourceSkeletalMeshComponent")
	{
		SetSourceSkeletalMeshComponent(SourceSkeletalMeshComponent);
		FString Error;
		RebuildStockingMesh(Error);
	}
	else if (Name == "StockingRegion")
	{
		SetStockingRegion(StockingRegion);
		FString Error;
		RebuildStockingMesh(Error);
	}
	else if (Name == "StockingMaterial")
	{
		SetMaterial(0, GetEffectiveStockingMaterial());
		SyncTearMaskBinding();
		MarkRenderStateDirty();
	}
	else if (Name == "SurfaceOffset" || Name == "BoundsPadding" || Name == "MaxPullDistance")
	{
		MarkBoneTransformsDirty();
		OnTransformDirty(ETeleportType::None);
	}
}

void UStockingComponent::SetSourceSkeletalMeshComponent(USkeletalMeshComponent* InSourceComponent)
{
	UnbindSourceImpactListener();
	SourceSkeletalMeshComponent = InSourceComponent;
	BindSourceImpactListener();
	bHasSyncedPoseState = false;
	LastSyncedSourceTransforms.Reset();
	MarkBoneTransformsDirty();
}

void UStockingComponent::CalibrateParametersForScale(float InScale)
{
	ReferenceScale = std::max(InScale, 0.05f);
	SurfaceOffset = 0.003f * ReferenceScale;
	PullRadius = 0.11f * ReferenceScale;
	MaxPullDistance = 0.075f * ReferenceScale;
	PullFalloffExponent = 2.15f;
	WrinkleAmplitude = 0.0075f * ReferenceScale;
	BoundsPadding = 0.04f * ReferenceScale;
	ClothJointWrinkleSlack = 0.0035f * ReferenceScale;
	ClothBackstopRadius = 0.025f * ReferenceScale;
	ClothSelfCollisionDistance = 0.0f;
	bNvClothParamsDirty = true;
	if (StockingRegion)
	{
		StockingRegion->SurfaceOffset = SurfaceOffset;
		StockingRegion->BoundaryFeatherDistance = 0.045f * ReferenceScale;
	}
	MarkBoneTransformsDirty();
	OnTransformDirty(ETeleportType::None);
}

void UStockingComponent::ResetClothSimulation()
{
	bNeedsClothTeleport = true;
	bClothSteppedThisFrame = false;
	bHasActiveClothWrinkles = false;
	bHasActiveClothPull = false;
	bClothAwakeLastFrame = false;
	PullReleaseEnvelope = 0.0f;
	LastClothPullMag = 0.0f;
	bHasSyncedPoseState = false;
	MarkBoneTransformsDirty();
}

void UStockingComponent::ApplyNvClothParams()
{
	bNvClothParamsDirty = true;
	if (!ClothInstance || !ClothFabric)
	{
		return;
	}

	FClothBridgeParams Params{};
	Params.Gravity = { 0.0f, 0.0f, 0.0f };
	Params.Damping = { ClothDamping, ClothDamping, ClothDamping };
	Params.LinearDrag = { 0.3f, 0.3f, 0.3f };
	Params.AngularDrag = { 0.3f, 0.3f, 0.3f };
	Params.LinearInertia = { 0.15f, 0.15f, 0.15f };
	Params.AngularInertia = { 0.1f, 0.1f, 0.1f };
	Params.CentrifugalInertia = { 0.05f, 0.05f, 0.05f };
	Params.SolverFrequency = 60.0f;
	Params.StiffnessFrequency = 60.0f;
	Params.StretchStiffness = std::clamp(ClothStretchStiffness, 0.05f, 1.0f);
	Params.BendStiffness = std::clamp(ClothBendStiffness, 0.0f, 1.0f);
	Params.ShearStiffness = std::clamp(ClothShearStiffness, 0.0f, 1.0f);
	Params.CompressionLimit = std::clamp(ClothCompressionLimit, 0.5f, 1.0f);
	Params.StretchLimit = std::clamp(ClothStretchLimit, 1.0f, 1.6f);
	Params.StiffnessMultiplier = 0.65f;
	Params.MotionConstraintStiffness = std::clamp(ClothMotionStiffness, 0.05f, 1.0f);
	Params.SelfCollisionDistance = std::max(ClothSelfCollisionDistance, 0.0f);
	Params.SelfCollisionStiffness = std::clamp(ClothSelfCollisionStiffness, 0.0f, 1.0f);
	Params.TetherScale = 1.0f;
	Params.TetherStiffness = 0.0f;
	Params.Friction = 0.25f;
	Params.bEnableContinuousCollision = false;

	ClothBridge_ApplyParams(ClothInstance, ClothFabric, &Params);
	bNvClothParamsDirty = false;
}

void UStockingComponent::SetStockingRegion(UStockingRegion* InRegion)
{
	StockingRegion = InRegion;
	if (StockingRegion)
	{
		SurfaceOffset = StockingRegion->SurfaceOffset;
		if (StockingRegion->bHasSavedPullParams)
		{
			PullRadius = StockingRegion->SavedPullRadius;
			MaxPullDistance = StockingRegion->SavedMaxPullDistance;
			PullFalloffExponent = StockingRegion->SavedPullFalloffExponent;
			WrinkleAmplitude = StockingRegion->SavedWrinkleAmplitude;
			WrinkleFrequency = StockingRegion->SavedWrinkleFrequency;
			SpringStiffness = StockingRegion->SavedSpringStiffness;
			SpringDamping = StockingRegion->SavedSpringDamping;
			BoundsPadding = StockingRegion->SavedBoundsPadding;
		}
		if (StockingRegion->bHasSavedClothWrinkleParams)
		{
			bUseNvClothSimulation = StockingRegion->SavedUseNvClothSimulation;
			ClothJointWrinkleSlack = StockingRegion->SavedClothJointWrinkleSlack;
			ClothBackstopRadius = StockingRegion->SavedClothBackstopRadius;
			ClothStretchStiffness = StockingRegion->SavedClothStretchStiffness;
			ClothBendStiffness = StockingRegion->SavedClothBendStiffness;
			ClothShearStiffness = StockingRegion->SavedClothShearStiffness;
			ClothCompressionLimit = StockingRegion->SavedClothCompressionLimit;
			ClothStretchLimit = StockingRegion->SavedClothStretchLimit;
			ClothMotionStiffness = StockingRegion->SavedClothMotionStiffness;
			ClothDamping = StockingRegion->SavedClothDamping;
			ClothSelfCollisionDistance = StockingRegion->SavedClothSelfCollisionDistance;
			ClothSelfCollisionStiffness = StockingRegion->SavedClothSelfCollisionStiffness;
			bNvClothParamsDirty = true;
		}
		if (StockingRegion->bHasSavedMaterialParams)
		{
			if (UMaterialInstanceDynamic* DefaultMat = GetDefaultStockingMaterialInstance())
			{
				FStockingMaterialLibrary::ApplyParameters(DefaultMat, StockingRegion->SavedMaterialParams);
			}
		}
	}
}

void UStockingComponent::SetStockingMaterial(UMaterialInterface* InMaterial)
{
	StockingMaterial = InMaterial;
	SetMaterial(0, GetEffectiveStockingMaterial());
	SyncTearMaskBinding();
	MarkRenderStateDirty();
}

UMaterialInstanceDynamic* UStockingComponent::GetDefaultStockingMaterialInstance()
{
	if (!DefaultStockingMaterial || !IsValid(DefaultStockingMaterial))
	{
		DefaultStockingMaterial = FStockingMaterialLibrary::CreateStockingMaterialInstance();
	}
	return DefaultStockingMaterial;
}

UMaterialInterface* UStockingComponent::GetEffectiveStockingMaterial()
{
	if (StockingMaterial && IsValid(StockingMaterial))
	{
		return StockingMaterial;
	}
	return GetDefaultStockingMaterialInstance();
}

bool UStockingComponent::ValidateSkeletonCompatibility(const USkeletalMesh& SourceMesh, const USkeletalMesh& StockingMesh, FString& OutError)
{
	OutError.clear();

	const TArray<FBoneInfo>& SourceBones = SourceMesh.GetRefSkeleton();
	const TArray<FBoneInfo>& StockingBones = StockingMesh.GetRefSkeleton();

	if (SourceBones.IsEmpty() || StockingBones.IsEmpty())
	{
		OutError = "Skeleton validation failed: skeleton is empty.";
		return false;
	}

	if (SourceBones.Num() != StockingBones.Num())
	{
		OutError = "Skeleton validation failed: bone count mismatch between source and stocking mesh.";
		return false;
	}

	for (int32 i = 0; i < SourceBones.Num(); ++i)
	{
		const FBoneInfo& Src = SourceBones[i];
		const FBoneInfo& Dst = StockingBones[i];

		if (Src.Name != Dst.Name || Src.ParentIndex != Dst.ParentIndex)
		{
			OutError = "Skeleton validation failed: bone order or hierarchy mismatch at bone '" + Src.Name + "'.";
			return false;
		}

		if (!AreVectorsClose(Src.LocalBindPose.Location, Dst.LocalBindPose.Location, 1e-3f) ||
			!AreQuatsClose(Src.LocalBindPose.Rotation, Dst.LocalBindPose.Rotation, 1e-3f) ||
			!AreVectorsClose(Src.LocalBindPose.Scale, Dst.LocalBindPose.Scale, 1e-3f) ||
			!AreMatricesClose(Src.InverseBindPose, Dst.InverseBindPose, 1e-2f))
		{
			OutError = "Skeleton validation failed: reference bind pose mismatch at bone '" + Src.Name + "'.";
			return false;
		}
	}

	return true;
}

bool UStockingComponent::ValidateAgainstSourceMesh(FString& OutError) const
{
	OutError.clear();
	if (!SourceSkeletalMeshComponent || !SourceSkeletalMeshComponent->GetSkeletalMesh())
	{
		OutError = "StockingComponent: source SkeletalMeshComponent or SkeletalMesh is missing.";
		return false;
	}

	if (!StockingRegion)
	{
		OutError = "StockingComponent: StockingRegion is not assigned.";
		return false;
	}

	const USkeletalMesh& SourceMesh = *SourceSkeletalMeshComponent->GetSkeletalMesh();
	if (!StockingRegion->ValidateAgainstMesh(SourceMesh, OutError))
	{
		return false;
	}

	if (GetSkeletalMesh())
	{
		if (!ValidateSkeletonCompatibility(SourceMesh, *GetSkeletalMesh(), OutError))
		{
			return false;
		}
	}

	return true;
}

bool UStockingComponent::RebuildStockingMesh(FString& OutError)
{
	OutError.clear();
	LastErrorMessage.clear();
	ResetPullImmediately();

	if (!SourceSkeletalMeshComponent || !SourceSkeletalMeshComponent->GetSkeletalMesh())
	{
		OutError = "StockingComponent: source SkeletalMeshComponent has no SkeletalMesh.";
		LastErrorMessage = OutError;
		KRS_LOG(Warning, "{}", OutError);
		SetSkeletalMesh(nullptr);
		return false;
	}

	if (!StockingRegion)
	{
		OutError = "StockingComponent: StockingRegion is null.";
		LastErrorMessage = OutError;
		KRS_LOG(Warning, "{}", OutError);
		SetSkeletalMesh(nullptr);
		return false;
	}

	const USkeletalMesh& SourceMesh = *SourceSkeletalMeshComponent->GetSkeletalMesh();
	UMaterialInterface* PreservedOverrideMat = (!StockingMaterial && GetOverrideMaterial(0) && GetOverrideMaterial(0)->GetBaseMaterial())
		? GetOverrideMaterial(0) : nullptr;
	ReferenceScale = std::max(UStockingRegion::ComputeReferenceScale(SourceMesh, 0), 0.05f);
	if (SurfaceOffset > 0.0f)
	{
		StockingRegion->SurfaceOffset = SurfaceOffset;
	}

	USkeletalMesh* NewStockingMesh = NewObject<USkeletalMesh>(this);
	if (!NewStockingMesh)
	{
		OutError = "StockingComponent: failed to allocate stocking mesh.";
		LastErrorMessage = OutError;
		return false;
	}

	if (!StockingRegion->BuildStockingMesh(SourceMesh, *NewStockingMesh, OutError))
	{
		LastErrorMessage = OutError;
		KRS_LOG(Warning, "{}", OutError);
		SetSkeletalMesh(nullptr);
		return false;
	}

	if (!ValidateSkeletonCompatibility(SourceMesh, *NewStockingMesh, OutError))
	{
		LastErrorMessage = OutError;
		KRS_LOG(Warning, "{}", OutError);
		SetSkeletalMesh(nullptr);
		return false;
	}

	GeneratedStockingMesh = NewStockingMesh;
	BakedSurfaceOffset = StockingRegion->SurfaceOffset;
	BuildRestAdjacency(*GeneratedStockingMesh);
	BuildCoveredBoneMask(*GeneratedStockingMesh);
	BindSourceImpactListener();
	bHasSyncedPoseState = false;
	LastSyncedSourceTransforms.Reset();
	if (!PreservedOverrideMat && !StockingMaterial && StockingRegion->bHasSavedMaterialParams)
	{
		if (UMaterialInstanceDynamic* DefaultMat = GetDefaultStockingMaterialInstance())
		{
			FStockingMaterialLibrary::ApplyParameters(DefaultMat, StockingRegion->SavedMaterialParams);
		}
	}
	SetSkeletalMesh(GeneratedStockingMesh);
	SetMaterial(0, PreservedOverrideMat ? PreservedOverrideMat : GetEffectiveStockingMaterial());
	RepairAllTears();
	BindDeformationCallback();
	MarkRenderStateDirty();
	UpdateBoneTransformsNow();
	OnTransformDirty(ETeleportType::None);
	return true;
}

void UStockingComponent::BeginPull(uint32 TriangleIndex, const FVector& Barycentric, const FVector& TargetWorldPosition)
{
	// 인게임에서는 당기기 기능을 비활성화하고 충돌 찢어짐만 유지
	if (IsInGameWorld(this))
	{
		return;
	}

	USkeletalMesh* Mesh = GetSkeletalMesh();
	if (!Mesh || Mesh->GetRenderData().LODRenderData.IsEmpty())
	{
		return;
	}

	const uint32 TriCount = static_cast<uint32>(Mesh->GetRenderData().LODRenderData[0].Indices.Num() / 3);
	if (TriangleIndex >= TriCount)
	{
		return;
	}

	GrabbedTriangleIndex = TriangleIndex;
	GrabBarycentric = Barycentric;
	PullTargetWorld = TargetWorldPosition;
	PullVelocityLocal = FVector::ZeroVector;
	bHasGrabAnchor = true;
	bIsPulling = true;
	ComputeGrabGeodesicDistances();
	BindDeformationCallback();
	MarkBoneTransformsDirty();
}

void UStockingComponent::UpdatePull(const FVector& TargetWorldPosition)
{
	if (!bIsPulling)
	{
		return;
	}
	PullTargetWorld = TargetWorldPosition;
	MarkBoneTransformsDirty();
}

void UStockingComponent::EndPull()
{
	bIsPulling = false;
	MarkBoneTransformsDirty();
}

void UStockingComponent::ResetPullImmediately()
{
	bIsPulling = false;
	bHasGrabAnchor = false;
	bHasActiveClothPull = false;
	PullReleaseEnvelope = 0.0f;
	LastClothPullMag = 0.0f;
	CurrentPullOffsetLocal = FVector::ZeroVector;
	PullVelocityLocal = FVector::ZeroVector;
	MaxAppliedDisplacement = 0.0f;
	MaxCurrentStretchRatio = 1.0f;
	bNeedsClothTeleport = true;
	MarkBoneTransformsDirty();
}

bool UStockingComponent::HasActiveDeformation() const
{
	return bIsPulling || bHasActiveClothPull || CurrentPullOffsetLocal.SizeSquared() > 1e-8f || PullVelocityLocal.SizeSquared() > 1e-8f;
}

FVector UStockingComponent::GetAnchorWorldPosition() const
{
	return GetWorldMatrix().TransformPosition(LastAnchorLocal);
}

void UStockingComponent::RefreshBoneTransforms()
{
	if (!SourceSkeletalMeshComponent || !GetSkeletalMesh())
	{
		Super::RefreshBoneTransforms();
		return;
	}

	const USkeletalMesh* SourceMesh = SourceSkeletalMeshComponent->GetSkeletalMesh();
	if (!SourceMesh)
	{
		Super::RefreshBoneTransforms();
		return;
	}

	const TArray<FMatrix>& SourceTransforms = SourceSkeletalMeshComponent->GetComponentSpaceTransform();
	if (SourceTransforms.Num() != GetSkeletalMesh()->GetNumBones())
	{
		FString Error;
		if (!ValidateSkeletonCompatibility(*SourceMesh, *GetSkeletalMesh(), Error))
		{
			LastErrorMessage = Error;
		}
		Super::RefreshBoneTransforms();
		return;
	}

	ComponentSpaceTransforms = SourceTransforms;
}

bool UStockingComponent::ShouldUsePostSkinDeform() const
{
	// 인게임에서는 관절 주름과 당기기 변형을 끄고 GPU 스키닝만 사용
	if (IsInGameWorld(this))
	{
		return false;
	}

	const bool bNvClothActive = bUseNvClothSimulation && ClothSolver && ClothInstance && ClothFabric;
	const bool bHasOffsetDelta = std::fabs(SurfaceOffset - BakedSurfaceOffset) > 1e-5f;
	return bNvClothActive || HasActiveDeformation() || bHasOffsetDelta;
}

bool UStockingComponent::HasPoseOrPullChanged(const TArray<FMatrix>& SourceTransforms, FSkeletalMeshObject* MeshObject) const
{
	const bool bIsGameWorld = IsInGameWorld(this);
	if (!bHasSyncedPoseState ||
		LastSyncedSourceComponent != SourceSkeletalMeshComponent ||
		LastSyncedMeshObject != MeshObject ||
		bLastSyncedUseNvCloth != bUseNvClothSimulation ||
		std::fabs(LastSyncedSurfaceOffset - SurfaceOffset) > 1e-6f ||
		(!bIsGameWorld && bUseNvClothSimulation && ClothSolver && ClothInstance && ClothFabric && bNeedsClothTeleport) ||
		bLastSyncedPulling != bIsPulling ||
		bLastSyncedHasGrabAnchor != bHasGrabAnchor ||
		LastSyncedGrabbedTriangleIndex != GrabbedTriangleIndex)
	{
		return true;
	}

	if ((CurrentPullOffsetLocal - LastSyncedPullOffsetLocal).SizeSquared() > 1e-10f ||
		(PullVelocityLocal - LastSyncedPullVelocityLocal).SizeSquared() > 1e-10f)
	{
		return true;
	}

	if (bIsPulling && (PullTargetWorld - LastSyncedPullTargetWorld).SizeSquared() > 1e-10f)
	{
		return true;
	}

	const int32 TransformCount = SourceTransforms.Num();
	if (LastSyncedSourceTransforms.Num() != TransformCount)
	{
		return true;
	}

	for (int32 i = 0; i < TransformCount; ++i)
	{
		if (!AreMatricesClose(SourceTransforms[i], LastSyncedSourceTransforms[i], 1e-5f))
		{
			return true;
		}
	}

	return false;
}

void UStockingComponent::RecordSyncedPoseState(const TArray<FMatrix>& SourceTransforms, FSkeletalMeshObject* MeshObject)
{
	bHasSyncedPoseState = true;
	LastSyncedSourceComponent = SourceSkeletalMeshComponent;
	LastSyncedMeshObject = MeshObject;
	LastSyncedSourceTransforms = SourceTransforms;
	bLastSyncedPulling = bIsPulling;
	bLastSyncedHasGrabAnchor = bHasGrabAnchor;
	bLastSyncedUseNvCloth = bUseNvClothSimulation;
	LastSyncedSurfaceOffset = SurfaceOffset;
	LastSyncedGrabbedTriangleIndex = GrabbedTriangleIndex;
	LastSyncedPullTargetWorld = PullTargetWorld;
	LastSyncedPullOffsetLocal = CurrentPullOffsetLocal;
	LastSyncedPullVelocityLocal = PullVelocityLocal;
}

void UStockingComponent::OnMeshObjectCreated()
{
	bHasSyncedPoseState = false;
	BindDeformationCallback();
}

void UStockingComponent::BindDeformationCallback()
{
	FSkeletalMeshObject* Object = GetMeshObject();
	if (!Object)
	{
		return;
	}

	const bool bNeedCallback = ShouldUsePostSkinDeform();
	const bool bHadCallback = Object->HasPostSkinDeformCallback();
	if (bNeedCallback == bHadCallback)
	{
		return;
	}

	if (bNeedCallback)
	{
		Object->SetPostSkinDeformCallback([this](TArray<FVertex>& InOutVertices, FBox& InOutBounds)
		{
			ApplyStockingDeformation(InOutVertices, InOutBounds);
		});
	}
	else
	{
		Object->SetPostSkinDeformCallback(nullptr);
	}
	MarkBoneTransformsDirty();
}

void UStockingComponent::BuildRestAdjacency(const USkeletalMesh& Mesh)
{
	WeldedVertexIds.Reset();
	WeldedVertexCount = 0;
	SimBindPositions.Reset();
	SimTriangleIndices.Reset();
	WeldedEdgeMasks.Reset();
	WeldedPrimaryUV.Reset();
	WeldedTileMinU.Reset();
	WeldedTileWidthU.Reset();
	WeldedTearFactor.Reset();
	WeldedAdjacency.Reset();
	GrabGeodesicDistances.Reset();

	if (Mesh.GetRenderData().LODRenderData.IsEmpty())
	{
		DestroyNvClothResources();
		return;
	}

	const FSkeletalMeshLODRenderData& LOD = Mesh.GetRenderData().LODRenderData[0];
	const int32 VertexCount = LOD.BindVertices.Num();
	WeldedVertexIds.SetNum(VertexCount);

	// 동일 좌표로 쪼개진 심 정점을 하나의 용접 정점으로 그룹화
	const float MeshScale = UStockingRegion::ComputeReferenceScale(Mesh, 0);
	const float QuantScale = 10000.0f / std::max(MeshScale, 0.05f);
	std::unordered_map<uint64, uint32> PosToWelded;
	PosToWelded.reserve(static_cast<size_t>(VertexCount));
	for (int32 v = 0; v < VertexCount; ++v)
	{
		const uint64 Key = MakeStockingPositionKey(LOD.BindVertices[v].Position, QuantScale);
		const std::unordered_map<uint64, uint32>::const_iterator Found = PosToWelded.find(Key);
		if (Found != PosToWelded.end())
		{
			const uint32 Wid = Found->second;
			WeldedVertexIds[v] = Wid;
			WeldedEdgeMasks[static_cast<int32>(Wid)] = std::min(
				WeldedEdgeMasks[static_cast<int32>(Wid)],
				LOD.BindVertices[v].Color.X);
			if (LOD.BindVertices[v].UV.X < WeldedPrimaryUV[static_cast<int32>(Wid)].X)
			{
				WeldedPrimaryUV[static_cast<int32>(Wid)] = LOD.BindVertices[v].UV;
			}
		}
		else
		{
			const uint32 NewId = WeldedVertexCount++;
			PosToWelded.emplace(Key, NewId);
			WeldedVertexIds[v] = NewId;
			SimBindPositions.Add(LOD.BindVertices[v].Position);
			WeldedEdgeMasks.Add(LOD.BindVertices[v].Color.X);
			WeldedPrimaryUV.Add(LOD.BindVertices[v].UV);
		}
	}

	WeldedAdjacency.SetNum(static_cast<int32>(WeldedVertexCount));

	const std::function<void(uint32, uint32)> AddNeighbor = [&](uint32 A, uint32 B)
	{
		if (A == B || A >= WeldedVertexCount || B >= WeldedVertexCount)
		{
			return;
		}
		TArray<FNeighborEdge>& List = WeldedAdjacency[static_cast<int32>(A)];
		for (int32 i = 0; i < List.Num(); ++i)
		{
			if (List[i].NeighborVertex == B)
			{
				return;
			}
		}
		const float Dist = (SimBindPositions[static_cast<int32>(A)] - SimBindPositions[static_cast<int32>(B)]).Size();
		if (Dist > 1e-6f)
		{
			List.Add({ B, Dist });
		}
	};

	SimTriangleIndices.Reserve(LOD.Indices.Num());
	for (int32 i = 0; i + 2 < LOD.Indices.Num(); i += 3)
	{
		const uint32 W0 = WeldedVertexIds[LOD.Indices[i]];
		const uint32 W1 = WeldedVertexIds[LOD.Indices[i + 1]];
		const uint32 W2 = WeldedVertexIds[LOD.Indices[i + 2]];
		AddNeighbor(W0, W1); AddNeighbor(W1, W0);
		AddNeighbor(W1, W2); AddNeighbor(W2, W1);
		AddNeighbor(W2, W0); AddNeighbor(W0, W2);
		if (W0 != W1 && W1 != W2 && W2 != W0)
		{
			SimTriangleIndices.Add(W0);
			SimTriangleIndices.Add(W1);
			SimTriangleIndices.Add(W2);
		}
	}

	ComputeComponentUVTiles(Mesh);
	RebuildNvClothFromSimMesh();
}

void UStockingComponent::BuildCoveredBoneMask(const USkeletalMesh& Mesh)
{
	CoveredBoneMask.Reset();
	const int32 BoneCount = Mesh.GetNumBones();
	if (BoneCount <= 0 || Mesh.GetRenderData().LODRenderData.IsEmpty())
	{
		return;
	}

	CoveredBoneMask.SetNum(BoneCount);
	for (int32 i = 0; i < BoneCount; ++i)
	{
		CoveredBoneMask[i] = 0;
	}

	const TArray<FSkinWeight>& Weights = Mesh.GetRenderData().LODRenderData[0].SkinWeights;
	for (int32 v = 0; v < Weights.Num(); ++v)
	{
		const FSkinWeight& SW = Weights[v];
		for (int32 k = 0; k < MaxBoneInfluences; ++k)
		{
			if (SW.Weight[k] > 0.15f && static_cast<int32>(SW.BoneIndex[k]) < BoneCount)
			{
				CoveredBoneMask[static_cast<int32>(SW.BoneIndex[k])] = 1;
			}
		}
	}
}

bool UStockingComponent::IsCoveredImpactBone(int32 BoneIndex) const
{
	if (BoneIndex < 0 || BoneIndex >= CoveredBoneMask.Num())
	{
		return true;
	}
	if (CoveredBoneMask[BoneIndex] != 0)
	{
		return true;
	}
	if (!SourceSkeletalMeshComponent || !SourceSkeletalMeshComponent->GetSkeletalMesh())
	{
		return false;
	}

	const TArray<FBoneInfo>& Bones = SourceSkeletalMeshComponent->GetSkeletalMesh()->GetRefSkeleton();
	const int32 NumBones = FMath::Min(Bones.Num(), CoveredBoneMask.Num());
	// 트위스트나 보조 본에 스킨 가중치가 몰린 경우 부모 물리 본의 충격도 커버된 것으로 본다
	for (int32 Child = BoneIndex + 1; Child < NumBones; ++Child)
	{
		if (CoveredBoneMask[Child] == 0)
		{
			continue;
		}
		for (int32 Parent = Bones[Child].ParentIndex; Parent >= 0; Parent = Bones[Parent].ParentIndex)
		{
			if (Parent == BoneIndex)
			{
				return true;
			}
			if (SourceSkeletalMeshComponent->GetBodyInstance(Bones[Parent].Name))
			{
				break;
			}
		}
	}
	return false;
}

void UStockingComponent::BindSourceImpactListener()
{
	if (BoundImpactSource == SourceSkeletalMeshComponent)
	{
		return;
	}

	UnbindSourceImpactListener();

	if (SourceSkeletalMeshComponent && IsValid(SourceSkeletalMeshComponent))
	{
		SourceSkeletalMeshComponent->AddPhysicsImpactListener(this, [this](const FSkeletalPhysicsImpact& Impact)
		{
			HandleSourcePhysicsImpact(Impact);
		});
		BoundImpactSource = SourceSkeletalMeshComponent;
	}
}

void UStockingComponent::UnbindSourceImpactListener()
{
	if (!BoundImpactSource)
	{
		return;
	}

	if (std::find(GUObjectArray.begin(), GUObjectArray.end(), BoundImpactSource) != GUObjectArray.end())
	{
		BoundImpactSource->RemovePhysicsImpactListener(this);
	}
	BoundImpactSource = nullptr;
}

void UStockingComponent::HandleSourcePhysicsImpact(const FSkeletalPhysicsImpact& Impact)
{
	if (!bEnableTearing || !bEnableRagdollImpactTearing || !SourceSkeletalMeshComponent)
	{
		return;
	}

	// 연결된 스켈레탈 메시의 래그돌 또는 바디 물리 시뮬레이션이 켜져 있을 때만 반응
	if (!SourceSkeletalMeshComponent->IsRagdollActive() && !SourceSkeletalMeshComponent->IsAnyBodySimulatingPhysics())
	{
		return;
	}

	// 스타킹이 덮고 있지 않은 본의 충돌은 즉시 무시
	if (!IsCoveredImpactBone(Impact.BoneIndex))
	{
		return;
	}

	ApplyWorldImpactTear(Impact.WorldLocation, Impact.WorldImpulse, Impact.WorldSlideVelocity, Impact.BoneIndex);
}

bool UStockingComponent::FindClosestTriangleToLocalPoint(
	const FVector& LocalPoint,
	float MaxSearchDist,
	uint32& OutTriangleIndex,
	FVector& OutBarycentric) const
{
	const USkeletalMesh* Mesh = GetSkeletalMesh();
	const FSkeletalMeshObject* MeshObject = GetMeshObject();
	if (!Mesh || !MeshObject || Mesh->GetRenderData().LODRenderData.IsEmpty())
	{
		return false;
	}

	const TArray<FVertex>* Vertices = MeshObject->GetCPUSkinnedVertices();
	if (!Vertices || Vertices->IsEmpty())
	{
		return false;
	}

	const TArray<uint32>& Indices = Mesh->GetRenderData().LODRenderData[0].Indices;
	const int32 TriCount = Indices.Num() / 3;
	float BestDistSq = MaxSearchDist * MaxSearchDist;
	bool bFound = false;

	for (int32 t = 0; t < TriCount; ++t)
	{
		const FVector& A = (*Vertices)[Indices[t * 3]].Position;
		const FVector& B = (*Vertices)[Indices[t * 3 + 1]].Position;
		const FVector& C = (*Vertices)[Indices[t * 3 + 2]].Position;

		const FVector AB = B - A;
		const FVector AC = C - A;
		const FVector AP = LocalPoint - A;
		const float D1 = AB.Dot(AP);
		const float D2 = AC.Dot(AP);

		float U = 0.0f;
		float V = 0.0f;
		float W = 0.0f;

		if (D1 <= 0.0f && D2 <= 0.0f)
		{
			U = 1.0f;
		}
		else
		{
			const FVector BP = LocalPoint - B;
			const float D3 = AB.Dot(BP);
			const float D4 = AC.Dot(BP);
			if (D3 >= 0.0f && D4 <= D3)
			{
				V = 1.0f;
			}
			else
			{
				const float VC = D1 * D4 - D3 * D2;
				if (VC <= 0.0f && D1 >= 0.0f && D3 <= 0.0f)
				{
					V = D1 / std::max(D1 - D3, 1e-8f);
					U = 1.0f - V;
				}
				else
				{
					const FVector CP = LocalPoint - C;
					const float D5 = AB.Dot(CP);
					const float D6 = AC.Dot(CP);
					if (D6 >= 0.0f && D5 <= D6)
					{
						W = 1.0f;
					}
					else
					{
						const float VB = D5 * D2 - D1 * D6;
						if (VB <= 0.0f && D2 >= 0.0f && D6 <= 0.0f)
						{
							W = D2 / std::max(D2 - D6, 1e-8f);
							U = 1.0f - W;
						}
						else
						{
							const float VA = D3 * D6 - D5 * D4;
							if (VA <= 0.0f && (D4 - D3) >= 0.0f && (D5 - D6) >= 0.0f)
							{
								const float S = (D4 - D3) / std::max((D4 - D3) + (D5 - D6), 1e-8f);
								V = 1.0f - S;
								W = S;
							}
							else
							{
								const float Denom = 1.0f / std::max(VA + VB + VC, 1e-8f);
								V = VB * Denom;
								W = VC * Denom;
								U = 1.0f - V - W;
							}
						}
					}
				}
			}
		}

		const FVector Closest = A * U + B * V + C * W;
		const float DistSq = (LocalPoint - Closest).SizeSquared();
		if (DistSq < BestDistSq)
		{
			BestDistSq = DistSq;
			OutTriangleIndex = static_cast<uint32>(t);
			OutBarycentric = FVector(U, V, W);
			bFound = true;
		}
	}

	return bFound;
}

bool UStockingComponent::ApplyWorldImpactTear(
	const FVector& WorldLocation,
	const FVector& WorldImpulse,
	const FVector& WorldSlideVelocity,
	int32 HitBoneIndex)
{
	if (!bEnableTearing || ImpactTearCooldownTimer > 0.0f)
	{
		return false;
	}

	if (!IsCoveredImpactBone(HitBoneIndex))
	{
		return false;
	}

	const float ImpulseMag = WorldImpulse.Size();
	const float SlideSpeed = WorldSlideVelocity.Size();
	const float EffectiveImpact = ImpulseMag * (1.0f + SlideSpeed * std::max(ImpactScrapeSensitivity, 0.0f));
	const float SafeThreshold = std::max(ImpactTearImpulseThreshold, 0.1f);
	if (EffectiveImpact < SafeThreshold)
	{
		return false;
	}

	const FMatrix InvWorld = GetWorldMatrix().Inverse();
	const FVector LocalPoint = InvWorld.TransformPosition(WorldLocation);
	const float MaxSearchDist = 0.25f * std::max(ReferenceScale, 0.05f);

	uint32 HitTri = 0;
	FVector HitBary(1.0f, 0.0f, 0.0f);
	if (!FindClosestTriangleToLocalPoint(LocalPoint, MaxSearchDist, HitTri, HitBary))
	{
		return false;
	}

	const FVector LocalSlide = InvWorld.TransformVector(WorldSlideVelocity);
	const FVector LocalImpulse = InvWorld.TransformVector(WorldImpulse);
	FVector TearDirLocal(0.0f, 0.0f, 1.0f);
	if (LocalSlide.SizeSquared() > 1e-4f)
	{
		TearDirLocal = LocalSlide.Normalized();
	}
	else if (LocalImpulse.SizeSquared() > 1e-6f)
	{
		TearDirLocal = LocalImpulse.Normalized();
	}

	const float Intensity = std::clamp((EffectiveImpact / SafeThreshold) * 0.75f, 0.45f, 2.2f);
	ApplyTearAtSurfaceHit(HitTri, HitBary, TearDirLocal, Intensity);
	ImpactTearCooldownTimer = std::max(ImpactTearCooldownDuration, 0.02f);
	return true;
}

void UStockingComponent::ComputeComponentUVTiles(const USkeletalMesh& Mesh)
{
	const int32 WeldedCount = static_cast<int32>(WeldedVertexCount);
	WeldedTileMinU.SetNum(WeldedCount);
	WeldedTileWidthU.SetNum(WeldedCount);
	WeldedTearFactor.SetNum(WeldedCount);
	for (int32 w = 0; w < WeldedCount; ++w)
	{
		WeldedTileMinU[w] = 0.0f;
		WeldedTileWidthU[w] = 1.0f;
		WeldedTearFactor[w] = 0.0f;
	}

	if (WeldedCount == 0 || WeldedPrimaryUV.Num() != WeldedCount || Mesh.GetRenderData().LODRenderData.IsEmpty())
	{
		return;
	}

	// 아틀라스 가로 분포의 빈 구간을 기준으로 좌우 다리 타일 범위를 정확히 분리
	constexpr int32 BinCount = 200;
	int32 Hist[BinCount] = {};
	for (int32 w = 0; w < WeldedCount; ++w)
	{
		const int32 Bin = std::clamp(static_cast<int32>(WeldedPrimaryUV[w].X * static_cast<float>(BinCount)), 0, BinCount - 1);
		Hist[Bin] += 1;
	}

	struct FUVInterval
	{
		float MinU = 0.0f;
		float MaxU = 1.0f;
	};
	TArray<FUVInterval> Intervals;
	int32 b = 0;
	while (b < BinCount)
	{
		if (Hist[b] == 0)
		{
			++b;
			continue;
		}
		const int32 StartBin = b;
		while (b < BinCount && Hist[b] > 0)
		{
			++b;
		}
		const int32 EndBin = b;
		const float MinU = static_cast<float>(StartBin) / static_cast<float>(BinCount);
		const float MaxU = static_cast<float>(EndBin) / static_cast<float>(BinCount);
		if ((MaxU - MinU) >= 0.04f)
		{
			Intervals.Add({ MinU, MaxU });
		}
	}

	if (Intervals.IsEmpty())
	{
		Intervals.Add({ 0.0f, 1.0f });
	}

	// 각 타일 구간 내 정점들의 실제 최소 최대 가로 범위 기록
	TArray<float> ExactMin;
	TArray<float> ExactMax;
	ExactMin.SetNum(Intervals.Num());
	ExactMax.SetNum(Intervals.Num());
	for (int32 i = 0; i < Intervals.Num(); ++i)
	{
		ExactMin[i] = FLT_MAX;
		ExactMax[i] = -FLT_MAX;
	}

	TArray<int32> VertexInterval;
	VertexInterval.SetNum(WeldedCount);
	for (int32 w = 0; w < WeldedCount; ++w)
	{
		const float U = WeldedPrimaryUV[w].X;
		int32 BestIdx = 0;
		float BestDist = FLT_MAX;
		for (int32 i = 0; i < Intervals.Num(); ++i)
		{
			float Dist = 0.0f;
			if (U < Intervals[i].MinU)
			{
				Dist = Intervals[i].MinU - U;
			}
			else if (U > Intervals[i].MaxU)
			{
				Dist = U - Intervals[i].MaxU;
			}
			if (Dist < BestDist)
			{
				BestDist = Dist;
				BestIdx = i;
			}
		}
		VertexInterval[w] = BestIdx;
		ExactMin[BestIdx] = std::min(ExactMin[BestIdx], U);
		ExactMax[BestIdx] = std::max(ExactMax[BestIdx], U);
	}

	for (int32 w = 0; w < WeldedCount; ++w)
	{
		const int32 Idx = VertexInterval[w];
		const float TileMin = (ExactMin[Idx] < ExactMax[Idx]) ? ExactMin[Idx] : Intervals[Idx].MinU;
		const float TileMax = (ExactMin[Idx] < ExactMax[Idx]) ? ExactMax[Idx] : Intervals[Idx].MaxU;
		WeldedTileMinU[w] = TileMin;
		WeldedTileWidthU[w] = std::max(TileMax - TileMin, 0.05f);
	}
}

void UStockingComponent::EnsureTearMaskTexture()
{
	constexpr uint32 TearMaskSize = 512u;
	if (TearMaskTexture && IsValid(TearMaskTexture) && TearMaskTexture->GetResource())
	{
		return;
	}

	TearMaskPixels.SetNum(static_cast<int32>(TearMaskSize * TearMaskSize * 4u));
	const int32 PixelCount = static_cast<int32>(TearMaskSize * TearMaskSize);
	std::fill_n(reinterpret_cast<uint32*>(TearMaskPixels.GetData()), PixelCount, 0x00800000u);
	TearMaskTexture = UTexture2D::CreateDynamicTexture(TearMaskSize, TearMaskSize, TearMaskPixels.GetData());
	SyncTearMaskBinding();
	MarkRenderStateDirty();
}

void UStockingComponent::SyncTearMaskBinding()
{
	UMaterialInterface* ActiveMat = GetEffectiveStockingMaterial();
	UMaterialInstanceDynamic* DynMat = Cast<UMaterialInstanceDynamic>(ActiveMat);
	if (!DynMat && ActiveMat && IsValid(ActiveMat))
	{
		DynMat = UMaterial::CreateDynamicInstance(ActiveMat);
		if (DynMat)
		{
			StockingMaterial = DynMat;
			SetMaterial(0, DynMat);
		}
	}

	UMaterialInstanceDynamic* SlotDynMat = Cast<UMaterialInstanceDynamic>(GetOverrideMaterial(0));
	UMaterialInstanceDynamic* Targets[2] = { DynMat, SlotDynMat };
	for (int32 i = 0; i < 2; ++i)
	{
		UMaterialInstanceDynamic* Target = Targets[i];
		if (!Target || !IsValid(Target) || (i == 1 && Target == DynMat))
		{
			continue;
		}
		if (TearMaskTexture && IsValid(TearMaskTexture))
		{
			Target->SetTextureParameterValue(FName("TearMaskTex"), TearMaskTexture);
		}
		Target->SetScalarParameterValue(FName("TearMaskActive"), TearNodes.IsEmpty() ? 0.0f : 1.0f);
	}
}

void UStockingComponent::RepairAllTears()
{
	const bool bHadTears = !TearNodes.IsEmpty();
	TearNodes.Reset();
	bTearMaskDirty = false;
	TearMaskUploadCooldown = 0.0f;
	PullTearCooldown = 0.0f;

	for (int32 w = 0; w < WeldedTearFactor.Num(); ++w)
	{
		WeldedTearFactor[w] = 0.0f;
	}

	EnsureTearMaskTexture();
	if (TearMaskTexture && IsValid(TearMaskTexture) && !TearMaskPixels.IsEmpty())
	{
		const int32 PixelCount = TearMaskPixels.Num() / 4;
		std::fill_n(reinterpret_cast<uint32*>(TearMaskPixels.GetData()), PixelCount, 0x00800000u);
		TearMaskTexture->UpdateDynamicPixels(TearMaskPixels.GetData(), TearMaskTexture->GetWidth() * 4u);
	}

	SyncTearMaskBinding();
	if (bHadTears)
	{
		MarkRenderStateDirty();
		MarkBoneTransformsDirty();
	}
}

float UStockingComponent::ComputeWrappedDeltaU(float U, float CenterU, float TileMinU, float TileWidthU) const
{
	const float SafeW = std::max(TileWidthU, 0.05f);
	float LocalU = U - TileMinU;
	float LocalC = CenterU - TileMinU;
	while (LocalU >= SafeW)
	{
		LocalU -= SafeW;
	}
	while (LocalU < 0.0f)
	{
		LocalU += SafeW;
	}
	while (LocalC >= SafeW)
	{
		LocalC -= SafeW;
	}
	while (LocalC < 0.0f)
	{
		LocalC += SafeW;
	}

	float Delta = LocalU - LocalC;
	if (Delta > SafeW * 0.5f)
	{
		Delta -= SafeW;
	}
	else if (Delta < -SafeW * 0.5f)
	{
		Delta += SafeW;
	}
	return Delta;
}

bool UStockingComponent::ComputeTriangleHitUV(
	uint32 TriangleIndex,
	const FVector& Barycentric,
	FVector2& OutUV,
	float& OutTileMinU,
	float& OutTileWidthU) const
{
	const USkeletalMesh* Mesh = GetSkeletalMesh();
	if (!Mesh || Mesh->GetRenderData().LODRenderData.IsEmpty())
	{
		return false;
	}

	const FSkeletalMeshLODRenderData& LOD = Mesh->GetRenderData().LODRenderData[0];
	const int32 TriCount = LOD.Indices.Num() / 3;
	if (static_cast<int32>(TriangleIndex) >= TriCount)
	{
		return false;
	}

	const uint32 I0 = LOD.Indices[TriangleIndex * 3];
	const uint32 I1 = LOD.Indices[TriangleIndex * 3 + 1];
	const uint32 I2 = LOD.Indices[TriangleIndex * 3 + 2];

	OutUV =
		LOD.BindVertices[I0].UV * Barycentric.X +
		LOD.BindVertices[I1].UV * Barycentric.Y +
		LOD.BindVertices[I2].UV * Barycentric.Z;

	const int32 W0 = static_cast<int32>(WeldedVertexIds[I0]);
	if (W0 >= 0 && W0 < WeldedTileMinU.Num())
	{
		OutTileMinU = WeldedTileMinU[W0];
		OutTileWidthU = WeldedTileWidthU[W0];
		const float SafeW = std::max(OutTileWidthU, 0.05f);
		float RelU = OutUV.X - OutTileMinU;
		while (RelU >= SafeW)
		{
			RelU -= SafeW;
		}
		while (RelU < 0.0f)
		{
			RelU += SafeW;
		}
		OutUV.X = OutTileMinU + RelU;
	}
	else
	{
		OutTileMinU = 0.0f;
		OutTileWidthU = 1.0f;
	}
	return true;
}

bool UStockingComponent::ComputeTriangleUVPullVector(
	uint32 TriangleIndex,
	const FVector& LocalPullVector,
	const TArray<FVector>* OptionalWeldedPositions,
	FVector2& OutPullUVDir,
	float& OutTangentialRatio) const
{
	OutPullUVDir = FVector2(0.0f, 0.0f);
	OutTangentialRatio = 0.0f;

	const float PullLen = LocalPullVector.Size();
	if (PullLen <= 1e-6f)
	{
		return false;
	}

	const USkeletalMesh* Mesh = GetSkeletalMesh();
	if (!Mesh || Mesh->GetRenderData().LODRenderData.IsEmpty())
	{
		return false;
	}

	const FSkeletalMeshLODRenderData& LOD = Mesh->GetRenderData().LODRenderData[0];
	const int32 TriCount = LOD.Indices.Num() / 3;
	if (static_cast<int32>(TriangleIndex) >= TriCount)
	{
		return false;
	}

	const uint32 I0 = LOD.Indices[TriangleIndex * 3];
	const uint32 I1 = LOD.Indices[TriangleIndex * 3 + 1];
	const uint32 I2 = LOD.Indices[TriangleIndex * 3 + 2];

	FVector P0 = LOD.BindVertices[I0].Position;
	FVector P1 = LOD.BindVertices[I1].Position;
	FVector P2 = LOD.BindVertices[I2].Position;
	if (OptionalWeldedPositions && !OptionalWeldedPositions->IsEmpty())
	{
		const int32 W0 = static_cast<int32>(WeldedVertexIds[I0]);
		const int32 W1 = static_cast<int32>(WeldedVertexIds[I1]);
		const int32 W2 = static_cast<int32>(WeldedVertexIds[I2]);
		if (W0 < OptionalWeldedPositions->Num() && W1 < OptionalWeldedPositions->Num() && W2 < OptionalWeldedPositions->Num())
		{
			P0 = (*OptionalWeldedPositions)[W0];
			P1 = (*OptionalWeldedPositions)[W1];
			P2 = (*OptionalWeldedPositions)[W2];
		}
	}
	else if (const FSkeletalMeshObject* MeshObject = GetMeshObject())
	{
		if (const TArray<FVertex>* SkinnedVerts = MeshObject->GetCPUSkinnedVertices();
			SkinnedVerts && static_cast<int32>(I2) < SkinnedVerts->Num() &&
			static_cast<int32>(I1) < SkinnedVerts->Num() && static_cast<int32>(I0) < SkinnedVerts->Num())
		{
			P0 = (*SkinnedVerts)[I0].Position;
			P1 = (*SkinnedVerts)[I1].Position;
			P2 = (*SkinnedVerts)[I2].Position;
		}
	}

	const FVector E1 = P1 - P0;
	const FVector E2 = P2 - P0;
	const FVector2 DUV1 = LOD.BindVertices[I1].UV - LOD.BindVertices[I0].UV;
	const FVector2 DUV2 = LOD.BindVertices[I2].UV - LOD.BindVertices[I0].UV;
	const float Det = DUV1.X * DUV2.Y - DUV1.Y * DUV2.X;

	FVector SurfaceNormal = FVector::Cross(E1, E2);
	if (SurfaceNormal.SizeSquared() > 1e-12f)
	{
		SurfaceNormal = SurfaceNormal.Normalized();
	}
	else
	{
		SurfaceNormal = FVector(1.0f, 0.0f, 0.0f);
	}

	// 삼각형의 가로 세로 접선 기저를 복원해 당김 벡터의 표면 방향 성분만 추출
	FVector TangentU(0.0f, 1.0f, 0.0f);
	FVector TangentV(0.0f, 0.0f, 1.0f);
	if (std::fabs(Det) > 1e-10f)
	{
		const float InvDet = 1.0f / Det;
		FVector RawU = (E1 * DUV2.Y - E2 * DUV1.Y) * InvDet;
		FVector RawV = (E2 * DUV1.X - E1 * DUV2.X) * InvDet;
		RawU = RawU - SurfaceNormal * RawU.Dot(SurfaceNormal);
		RawV = RawV - SurfaceNormal * RawV.Dot(SurfaceNormal);
		if (RawU.SizeSquared() > 1e-10f)
		{
			TangentU = RawU.Normalized();
		}
		if (RawV.SizeSquared() > 1e-10f)
		{
			TangentV = RawV.Normalized();
		}
	}
	else
	{
		FVector FallbackV = FVector(0.0f, 0.0f, 1.0f) - SurfaceNormal * SurfaceNormal.Z;
		if (FallbackV.SizeSquared() > 1e-10f)
		{
			TangentV = FallbackV.Normalized();
		}
		TangentU = FVector::Cross(TangentV, SurfaceNormal).Normalized();
	}

	const FVector PullNorm = LocalPullVector / PullLen;
	const float CompU = PullNorm.Dot(TangentU);
	const float CompV = PullNorm.Dot(TangentV);
	const float TanMag = std::sqrt(CompU * CompU + CompV * CompV);
	OutTangentialRatio = std::clamp(TanMag, 0.0f, 1.0f);
	if (TanMag > 1e-4f)
	{
		OutPullUVDir = FVector2(CompU / TanMag, CompV / TanMag);
	}
	return true;
}

int32 UStockingComponent::FindNearestTearIndex(
	const FVector2& UV,
	float TileMinU,
	float TileWidthU,
	float MaxNormalizedDist) const
{
	int32 BestIdx = -1;
	float BestDist = MaxNormalizedDist;
	for (int32 i = 0; i < TearNodes.Num(); ++i)
	{
		const FStockingTearNode& Node = TearNodes[i];
		if (std::fabs(Node.TileMinU - TileMinU) > 0.04f)
		{
			continue;
		}
		const float DU = ComputeWrappedDeltaU(UV.X, Node.CenterUV.X, TileMinU, TileWidthU);
		const float DV = UV.Y - Node.CenterUV.Y;
		const float ScaleU = std::max(Node.RadiusU, 0.015f);
		const float ScaleV = std::max(Node.RadiusV, 0.032f);
		const float NormDist = std::sqrt((DU / ScaleU) * (DU / ScaleU) + (DV / ScaleV) * (DV / ScaleV));
		if (NormDist < BestDist)
		{
			BestDist = NormDist;
			BestIdx = i;
		}
	}
	return BestIdx;
}

void UStockingComponent::AddOrExpandTearAtUV(
	const FVector2& UV,
	float TileMinU,
	float TileWidthU,
	const FVector2& PullDirUV,
	float TangentialRatio,
	float SpreadAmount,
	bool bAllowNewNode)
{
	constexpr int32 MaxTearNodes = 28;
	const float SafeSpread = std::clamp(SpreadAmount, 0.05f, 3.0f);
	const float SafeMaxRun = std::clamp(TearRunLength, 0.04f, 0.75f);
	const float SafeW = std::max(TileWidthU, 0.05f);
	const bool bWasEmpty = TearNodes.IsEmpty();

	// 정면 수직 당김은 기울기 없는 세로 타원으로 좌우 대각 당김은 해당 방향으로 기울어지게 계산
	const float TanStrength = std::clamp((TangentialRatio - 0.14f) / 0.62f, 0.0f, 1.0f);
	const float VerticalSign = (PullDirUV.Y < -0.20f) ? -1.0f : 1.0f;
	const float RawTilt = std::atan2(-PullDirUV.X * VerticalSign, std::fabs(PullDirUV.Y) + 0.58f);
	const float DesiredTilt = std::clamp(RawTilt * TanStrength, -0.82f, 0.82f);
	const FVector2 DesiredBias(PullDirUV.X * TanStrength, PullDirUV.Y * TanStrength);

	const int32 NearestIdx = FindNearestTearIndex(UV, TileMinU, TileWidthU, 2.35f);
	if (NearestIdx >= 0)
	{
		FStockingTearNode& Node = TearNodes[NearestIdx];
		const float HorizPull = std::fabs(PullDirUV.X) * TanStrength;
		const float VertPull = std::fabs(PullDirUV.Y) * TanStrength;
		const float DirWeightU = 0.55f + 0.85f * HorizPull;
		const float DirWeightV = 0.70f + 0.75f * VertPull + 0.25f * (1.0f - TanStrength);
		Node.RadiusU = std::min(Node.RadiusU + SafeSpread * 0.0085f * DirWeightU, SafeW * 0.11f);
		Node.RadiusV = std::min(Node.RadiusV + SafeSpread * 0.024f * DirWeightV, 0.14f);
		Node.OpenStrength = std::min(Node.OpenStrength + SafeSpread * 0.24f, 1.32f);

		const float TiltBlend = std::clamp(0.18f * SafeSpread, 0.05f, 0.35f);
		Node.TiltAngle = Node.TiltAngle * (1.0f - TiltBlend) + DesiredTilt * TiltBlend;
		Node.PullBiasUV = FVector2(
			Node.PullBiasUV.X * (1.0f - TiltBlend) + DesiredBias.X * TiltBlend,
			Node.PullBiasUV.Y * (1.0f - TiltBlend) + DesiredBias.Y * TiltBlend);

		// 가장자리 당김 방향으로 구멍 중심을 이동해 왼쪽 오른쪽 정면 당김의 전개 방향 구분
		const float DeltaU = ComputeWrappedDeltaU(UV.X, Node.CenterUV.X, TileMinU, TileWidthU);
		const float DeltaV = UV.Y - Node.CenterUV.Y;
		float NextRelU = (Node.CenterUV.X - TileMinU) + DeltaU * (0.10f * SafeSpread) + DesiredBias.X * (0.006f * SafeSpread);
		while (NextRelU >= SafeW)
		{
			NextRelU -= SafeW;
		}
		while (NextRelU < 0.0f)
		{
			NextRelU += SafeW;
		}
		Node.CenterUV.X = TileMinU + NextRelU;
		Node.CenterUV.Y = std::clamp(Node.CenterUV.Y + DeltaV * (0.14f * SafeSpread) + DesiredBias.Y * (0.010f * SafeSpread), 0.03f, 0.97f);

		const float UpBias = 1.0f + std::max(DesiredBias.Y, 0.0f) * 0.65f;
		const float DownBias = 1.0f + std::max(-DesiredBias.Y, 0.0f) * 0.65f;
		Node.RunUpV = std::min(Node.RunUpV + SafeSpread * 0.12f * UpBias, SafeMaxRun);
		Node.RunDownV = std::min(Node.RunDownV + SafeSpread * 0.12f * DownBias, SafeMaxRun);
		Node.RunWidthU = std::max(Node.RunWidthU, Node.RadiusU * 1.10f);

		// 세로 올 풀림 줄기를 따라 현재 구멍 기울기에 맞춰 보조 타원 구멍을 연쇄 생성
		if (!Node.bSpawnedUpperSubTear && Node.RunUpV > 0.12f && TearNodes.Num() < MaxTearNodes)
		{
			Node.bSpawnedUpperSubTear = true;
			const float OffsetV = std::max(Node.RadiusV * 1.38f, 0.032f);
			const float OffsetU = -std::sin(Node.TiltAngle) * OffsetV * 0.32f;
			float SubRelU = (Node.CenterUV.X - TileMinU) + OffsetU;
			while (SubRelU >= SafeW) { SubRelU -= SafeW; }
			while (SubRelU < 0.0f) { SubRelU += SafeW; }
			FStockingTearNode Sub{};
			Sub.CenterUV = FVector2(
				TileMinU + SubRelU,
				std::clamp(Node.CenterUV.Y + OffsetV, 0.04f, 0.96f));
			Sub.RadiusU = std::max(Node.RadiusU * 0.72f, 0.0055f);
			Sub.RadiusV = std::max(Node.RadiusV * 0.64f, 0.018f);
			Sub.RunUpV = SafeMaxRun * 0.70f;
			Sub.RunDownV = OffsetV * 0.95f;
			Sub.RunWidthU = std::max(Node.RunWidthU * 0.92f, Sub.RadiusU * 1.10f);
			Sub.OpenStrength = 0.78f;
			Sub.TiltAngle = Node.TiltAngle * 0.90f;
			Sub.PullBiasUV = Node.PullBiasUV * 0.75f;
			Sub.Seed = Node.Seed + 2.39996f;
			Sub.TileMinU = TileMinU;
			Sub.TileWidthU = SafeW;
			Sub.bSpawnedLowerSubTear = true;
			TearNodes.Add(Sub);
		}
		if (!Node.bSpawnedLowerSubTear && Node.RunDownV > 0.12f && TearNodes.Num() < MaxTearNodes)
		{
			Node.bSpawnedLowerSubTear = true;
			const float OffsetV = std::max(Node.RadiusV * 1.38f, 0.032f);
			const float OffsetU = std::sin(Node.TiltAngle) * OffsetV * 0.32f;
			float SubRelU = (Node.CenterUV.X - TileMinU) + OffsetU;
			while (SubRelU >= SafeW) { SubRelU -= SafeW; }
			while (SubRelU < 0.0f) { SubRelU += SafeW; }
			FStockingTearNode Sub{};
			Sub.CenterUV = FVector2(
				TileMinU + SubRelU,
				std::clamp(Node.CenterUV.Y - OffsetV, 0.04f, 0.96f));
			Sub.RadiusU = std::max(Node.RadiusU * 0.72f, 0.0055f);
			Sub.RadiusV = std::max(Node.RadiusV * 0.64f, 0.018f);
			Sub.RunUpV = OffsetV * 0.95f;
			Sub.RunDownV = SafeMaxRun * 0.70f;
			Sub.RunWidthU = std::max(Node.RunWidthU * 0.92f, Sub.RadiusU * 1.10f);
			Sub.OpenStrength = 0.78f;
			Sub.TiltAngle = Node.TiltAngle * 0.90f;
			Sub.PullBiasUV = Node.PullBiasUV * 0.75f;
			Sub.Seed = Node.Seed + 4.79992f;
			Sub.TileMinU = TileMinU;
			Sub.TileWidthU = SafeW;
			Sub.bSpawnedUpperSubTear = true;
			TearNodes.Add(Sub);
		}

		bTearMaskDirty = true;
	}
	else if (bAllowNewNode && TearNodes.Num() < MaxTearNodes)
	{
		FStockingTearNode NewNode{};
		NewNode.CenterUV = FVector2(UV.X, std::clamp(UV.Y, 0.03f, 0.97f));
		const float HorizFactor = std::fabs(PullDirUV.X) * TanStrength;
		const float InitRu = std::clamp(TearBrushRadiusUV * (0.56f + 0.34f * HorizFactor) * SafeSpread, 0.0060f, SafeW * 0.060f);
		const float AspectRatio = 2.75f - 0.85f * HorizFactor;
		NewNode.RadiusU = InitRu;
		NewNode.RadiusV = std::min(InitRu * AspectRatio, 0.072f);
		NewNode.RunUpV = SafeMaxRun * (0.55f + 0.25f * std::max(DesiredBias.Y, 0.0f));
		NewNode.RunDownV = SafeMaxRun * (0.55f + 0.25f * std::max(-DesiredBias.Y, 0.0f));
		NewNode.RunWidthU = InitRu * 1.15f;
		NewNode.OpenStrength = 0.85f;
		NewNode.TiltAngle = DesiredTilt;
		NewNode.PullBiasUV = DesiredBias;
		NewNode.Seed = 1.7f + static_cast<float>(TearNodes.Num()) * 2.39996f;
		NewNode.TileMinU = TileMinU;
		NewNode.TileWidthU = SafeW;
		TearNodes.Add(NewNode);
		bTearMaskDirty = true;
	}

	if (bWasEmpty && !TearNodes.IsEmpty())
	{
		EnsureTearMaskTexture();
		SyncTearMaskBinding();
		MarkRenderStateDirty();
	}
}

void UStockingComponent::ApplyTearAtSurfaceHit(
	uint32 TriangleIndex,
	const FVector& Barycentric,
	const FVector& DragDirLocal,
	float Intensity)
{
	if (!bEnableTearing)
	{
		return;
	}

	FVector2 HitUV;
	float TileMinU = 0.0f;
	float TileWidthU = 1.0f;
	if (!ComputeTriangleHitUV(TriangleIndex, Barycentric, HitUV, TileMinU, TileWidthU))
	{
		return;
	}

	FVector2 DirUV(0.0f, 0.0f);
	float TangentialRatio = 0.0f;
	ComputeTriangleUVPullVector(TriangleIndex, DragDirLocal, nullptr, DirUV, TangentialRatio);
	AddOrExpandTearAtUV(HitUV, TileMinU, TileWidthU, DirUV, TangentialRatio, std::max(Intensity, 0.25f) * TearSpreadSpeed, true);
	if (bTearMaskDirty && TearMaskUploadCooldown <= 0.0f)
	{
		RebuildTearMaskTexture();
	}
	MarkBoneTransformsDirty();
}

void UStockingComponent::UpdatePullTearing(
	const FSkeletalMeshLODRenderData& LOD,
	const TArray<FVector>& DeformedWeldedPos,
	const TArray<float>& WeldedStretch)
{
	if (!bEnableTearing || !bIsPulling || !bHasGrabAnchor)
	{
		return;
	}

	const int32 TriCount = LOD.Indices.Num() / 3;
	if (static_cast<int32>(GrabbedTriangleIndex) >= TriCount)
	{
		return;
	}

	const uint32 I0 = LOD.Indices[GrabbedTriangleIndex * 3];
	const uint32 I1 = LOD.Indices[GrabbedTriangleIndex * 3 + 1];
	const uint32 I2 = LOD.Indices[GrabbedTriangleIndex * 3 + 2];
	const int32 W0 = static_cast<int32>(WeldedVertexIds[I0]);
	const int32 W1 = static_cast<int32>(WeldedVertexIds[I1]);
	const int32 W2 = static_cast<int32>(WeldedVertexIds[I2]);

	const float LocalStretch = std::max(
		MaxCurrentStretchRatio * 0.92f,
		std::max(WeldedStretch[W0], std::max(WeldedStretch[W1], WeldedStretch[W2])));

	FVector2 AnchorUV;
	float TileMinU = 0.0f;
	float TileWidthU = 1.0f;
	if (!ComputeTriangleHitUV(GrabbedTriangleIndex, GrabBarycentric, AnchorUV, TileMinU, TileWidthU))
	{
		return;
	}

	const int32 NearTear = FindNearestTearIndex(AnchorUV, TileMinU, TileWidthU, 2.35f);
	const float EffectiveThreshold = (NearTear >= 0)
		? std::max(1.06f, TearStretchThreshold * 0.80f)
		: std::max(1.08f, TearStretchThreshold);

	if (LocalStretch <= EffectiveThreshold)
	{
		return;
	}

	FVector2 PullUVDir(0.0f, 0.0f);
	float TangentialRatio = 0.0f;
	ComputeTriangleUVPullVector(GrabbedTriangleIndex, CurrentPullOffsetLocal, &DeformedWeldedPos, PullUVDir, TangentialRatio);

	const float Excess = LocalStretch - EffectiveThreshold;
	const float SpreadStep = std::clamp(Excess * 1.85f * TearSpreadSpeed * PendingClothDeltaTime * 6.0f, 0.05f, 0.65f);
	const bool bCanCreateNew = (PullTearCooldown <= 0.0f);

	AddOrExpandTearAtUV(AnchorUV, TileMinU, TileWidthU, PullUVDir, TangentialRatio, SpreadStep, bCanCreateNew);
	if (NearTear < 0 && bCanCreateNew)
	{
		PullTearCooldown = 0.22f;
	}
}

void UStockingComponent::RebuildTearMaskTexture()
{
	EnsureTearMaskTexture();
	if (!TearMaskTexture || !IsValid(TearMaskTexture))
	{
		bTearMaskDirty = false;
		return;
	}

	const int32 Size = static_cast<int32>(TearMaskTexture->GetWidth());
	if (Size <= 0 || TearMaskPixels.Num() != Size * Size * 4)
	{
		bTearMaskDirty = false;
		return;
	}

	const int32 PixelCount = Size * Size;
	std::fill_n(reinterpret_cast<uint32*>(TearMaskPixels.GetData()), PixelCount, 0x00800000u);

	const float InvSize = 1.0f / static_cast<float>(Size);
	const float EdgeBandUV = 0.0024f;
	const float InnerShadowBandUV = 0.0024f;
	const float MaxPushEncode = 0.075f;
	const float NeedleColSpacing = 0.0024f;

	for (int32 n = 0; n < TearNodes.Num(); ++n)
	{
		const FStockingTearNode& Node = TearNodes[n];
		const float SafeW = std::max(Node.TileWidthU, 0.05f);
		const float Ru = std::max(Node.RadiusU, 0.003f);
		const float Rv = std::max(Node.RadiusV, 0.008f);
		const float RunW = std::max(Node.RunWidthU, Ru * 1.06f);
		const float PushReachU = Ru * 1.85f;
		const float PushReachV = Rv * 1.95f;
		const float BoundU = std::max(PushReachU, RunW * 1.25f);

		const float CosT = std::cos(Node.TiltAngle);
		const float SinT = std::sin(Node.TiltAngle);

		const float MinV = std::clamp(Node.CenterUV.Y - std::max(PushReachV, Node.RunDownV), 0.0f, 1.0f);
		const float MaxV = std::clamp(Node.CenterUV.Y + std::max(PushReachV, Node.RunUpV), 0.0f, 1.0f);
		const int32 Y0 = std::clamp(static_cast<int32>(std::floor(MinV * static_cast<float>(Size))), 0, Size - 1);
		const int32 Y1 = std::clamp(static_cast<int32>(std::ceil(MaxV * static_cast<float>(Size))), 0, Size - 1);

		// 타일 전체 대신 구멍의 실제 가로 영향 구간만 순회해 CPU 부하 최소화
		const float TileMaxU = Node.TileMinU + SafeW * 1.10f;
		int32 SpanX0[3] = {};
		int32 SpanX1[3] = {};
		int32 SpanCount = 0;
		const std::function<void(float, float)> AddXSpan = [&](float MinUVal, float MaxUVal)
		{
			const float ClampedMin = std::max(MinUVal, Node.TileMinU);
			const float ClampedMax = std::min(MaxUVal, TileMaxU);
			if (ClampedMin > ClampedMax || SpanCount >= 3)
			{
				return;
			}
			const int32 IX0 = std::clamp(static_cast<int32>(std::floor(ClampedMin * static_cast<float>(Size))), 0, Size - 1);
			const int32 IX1 = std::clamp(static_cast<int32>(std::ceil(ClampedMax * static_cast<float>(Size))), 0, Size - 1);
			if (IX0 <= IX1)
			{
				SpanX0[SpanCount] = IX0;
				SpanX1[SpanCount] = IX1;
				++SpanCount;
			}
		};

		AddXSpan(Node.CenterUV.X - BoundU, Node.CenterUV.X + BoundU);
		if (Node.CenterUV.X - BoundU < Node.TileMinU)
		{
			AddXSpan(Node.CenterUV.X + SafeW - BoundU, TileMaxU);
		}
		if (Node.CenterUV.X + BoundU > Node.TileMinU + SafeW)
		{
			AddXSpan(Node.TileMinU, Node.CenterUV.X - SafeW + BoundU);
		}

		for (int32 y = Y0; y <= Y1; ++y)
		{
			const float V = (static_cast<float>(y) + 0.5f) * InvSize;
			const float DV = V - Node.CenterUV.Y;
			const bool bInRunV = (DV >= -Node.RunDownV && DV <= Node.RunUpV);
			const float RunFracV = (DV >= 0.0f)
				? (Node.RunUpV > 1e-4f ? (DV / Node.RunUpV) : 1.0f)
				: (Node.RunDownV > 1e-4f ? (-DV / Node.RunDownV) : 1.0f);
			const float CurRunW = RunW * std::clamp(1.0f - RunFracV * 0.42f, 0.28f, 1.0f);

			for (int32 sp = 0; sp < SpanCount; ++sp)
			{
				for (int32 x = SpanX0[sp]; x <= SpanX1[sp]; ++x)
				{
					const float U = (static_cast<float>(x) + 0.5f) * InvSize;
					const float DU = ComputeWrappedDeltaU(U, Node.CenterUV.X, Node.TileMinU, SafeW);
					if (std::fabs(DU) > BoundU)
					{
						continue;
					}

					// 당김 방향으로 기울어지고 해당 측면이 더 길게 벌어지는 비대칭 타원 거리 계산
					const float LX = DU * CosT + DV * SinT;
					const float LY = -DU * SinT + DV * CosT;
					const float RawNormX = LX / Ru;
					const float RawNormY = LY / Rv;
					const float RawEllipR = std::sqrt(RawNormX * RawNormX + RawNormY * RawNormY);
					const float PullSideDot = (RawEllipR > 1e-4f)
						? ((DU / Ru) * Node.PullBiasUV.X + (DV / Rv) * Node.PullBiasUV.Y) / RawEllipR
						: 0.0f;
					const float SideStretch = 1.0f + 0.28f * std::clamp(PullSideDot, -0.55f, 1.0f);
					const float EllipR = RawEllipR / SideStretch;
					const float MinAxis = std::min(Ru, Rv);
					const float SignedDistUV = (EllipR - 1.0f) * MinAxis;

					float HoleVal = 0.0f;
					float InnerRimVal = 0.0f;
					if (SignedDistUV < EdgeBandUV * 1.4f)
					{
						HoleVal = std::clamp(0.5f - SignedDistUV / (EdgeBandUV * 2.0f), 0.0f, 1.0f);

						// 구멍 바로 안쪽 얇은 테두리에 방향성 피부 볼록 접촉 음영 생성
						if (SignedDistUV < 0.0f && SignedDistUV > -InnerShadowBandUV)
						{
							const float DepthT = (-SignedDistUV) / InnerShadowBandUV;
							const float DirBias = 0.42f + 0.58f * std::clamp(0.5f + 0.5f * (RawNormX * 0.72f - RawNormY * 0.69f), 0.0f, 1.0f);
							InnerRimVal = (1.0f - DepthT) * (1.0f - DepthT) * DirBias;
						}
					}

					// 구멍이 실을 위아래 곡선 경계와 당긴 기울기 방향으로 밀어내는 연속 변위 필드 누적
					float PushDisp = 0.0f;
					if (EllipR < 2.2f)
					{
						const float RadialDecay = (EllipR <= 1.0f)
							? 1.0f
							: std::exp(-(EllipR - 1.0f) * 2.35f);
						const float LatEnvelope = std::exp(-(RawNormX * RawNormX) * 0.65f);
						const float SmoothSignY = LY / std::sqrt(LY * LY + (Rv * 0.24f) * (Rv * 0.24f));
						const float LocalTiltShear = -SinT * DU * 0.75f * RadialDecay * LatEnvelope;
						PushDisp = SmoothSignY * (Rv * 0.92f) * RadialDecay * LatEnvelope + LocalTiltShear;
					}

					// 구멍들을 하나로 잇는 응집된 세로 올 풀림 리본과 상하 바늘형 끝단 생성
					float RunVal = 0.0f;
					if (bInRunV && std::fabs(DU) < CurRunW)
					{
						const float EdgeJitter = 0.0014f * std::sin(V * 240.0f + Node.Seed);
						const float AbsDU = std::fabs(DU + EdgeJitter);
						const float LatRatio = AbsDU / std::max(CurRunW, 1e-4f);
						if (LatRatio < 1.0f)
						{
							const float RibbonMask = std::clamp((1.0f - LatRatio) / 0.14f, 0.0f, 1.0f);
							// 상하 끝단으로 갈수록 여러 가닥 바늘처럼 갈라지며 자연스럽게 소멸
							const float ColF = DU / NeedleColSpacing;
							const float ColIdx = std::floor(ColF + 0.5f);
							const float ColFrac = std::fabs(ColF - ColIdx) * 2.0f;
							const float HashRaw = std::sin(ColIdx * 12.9898f + Node.Seed * 78.233f) * 43758.5453f;
							const float ColHash = HashRaw - std::floor(HashRaw);
							const float NeedleReach = std::clamp(0.32f + 0.68f * ColHash * std::exp(-LatRatio * LatRatio * 1.8f), 0.18f, 1.0f);
							if (RunFracV < 0.28f)
							{
								RunVal = RibbonMask;
							}
							else if (RunFracV < NeedleReach)
							{
								const float TipFade = std::clamp((NeedleReach - RunFracV) / std::max(NeedleReach * 0.25f, 0.05f), 0.0f, 1.0f);
								const float NeedleBlend = std::clamp((RunFracV - 0.28f) / 0.22f, 0.0f, 1.0f);
								const float WaleStrands = std::clamp((0.84f - ColFrac) / 0.22f, 0.0f, 1.0f);
								RunVal = RibbonMask * TipFade * (1.0f - NeedleBlend + NeedleBlend * WaleStrands);
							}
						}
					}

					if (HoleVal <= 0.002f && RunVal <= 0.002f && InnerRimVal <= 0.002f && std::fabs(PushDisp) <= 1e-5f)
					{
						continue;
					}

					const int32 PixIdx = (y * Size + x) * 4;
					const float PrevHole = static_cast<float>(TearMaskPixels[PixIdx]) / 255.0f;
					const float PrevRun = static_cast<float>(TearMaskPixels[PixIdx + 1]) / 255.0f;
					const float PrevPush = ((static_cast<float>(TearMaskPixels[PixIdx + 2]) / 255.0f) - 0.5f) * (2.0f * MaxPushEncode);
					const float PrevInner = static_cast<float>(TearMaskPixels[PixIdx + 3]) / 255.0f;

					const float MergedHole = std::max(PrevHole, HoleVal);
					const float MergedRun = std::max(PrevRun, RunVal);
					const float MergedPush = std::clamp(PrevPush + PushDisp, -MaxPushEncode, MaxPushEncode);
					const float MergedInner = (MergedHole > 0.92f)
						? std::min(PrevInner, InnerRimVal)
						: std::max(PrevInner, InnerRimVal);

					TearMaskPixels[PixIdx] = static_cast<uint8>(std::clamp(MergedHole * 255.0f, 0.0f, 255.0f));
					TearMaskPixels[PixIdx + 1] = static_cast<uint8>(std::clamp(MergedRun * 255.0f, 0.0f, 255.0f));
					TearMaskPixels[PixIdx + 2] = static_cast<uint8>(std::clamp((0.5f + MergedPush / (2.0f * MaxPushEncode)) * 255.0f, 0.0f, 255.0f));
					TearMaskPixels[PixIdx + 3] = static_cast<uint8>(std::clamp(MergedInner * 255.0f, 0.0f, 255.0f));
				}
			}
		}
	}

	TearMaskTexture->UpdateDynamicPixels(TearMaskPixels.GetData(), static_cast<uint32>(Size * 4));
	SyncTearMaskBinding();

	// 찢김 주변 용접 정점의 물리 여유 가중치 갱신
	const int32 WeldedCount = static_cast<int32>(WeldedVertexCount);
	if (WeldedTearFactor.Num() == WeldedCount && WeldedPrimaryUV.Num() == WeldedCount)
	{
		for (int32 w = 0; w < WeldedCount; ++w)
		{
			float Factor = 0.0f;
			const FVector2& UV = WeldedPrimaryUV[w];
			for (int32 n = 0; n < TearNodes.Num(); ++n)
			{
				const FStockingTearNode& Node = TearNodes[n];
				if (std::fabs(Node.TileMinU - WeldedTileMinU[w]) > 0.04f)
				{
					continue;
				}
				const float DU = ComputeWrappedDeltaU(UV.X, Node.CenterUV.X, Node.TileMinU, Node.TileWidthU) / std::max(Node.RadiusU * 2.2f, 0.02f);
				const float DV = (UV.Y - Node.CenterUV.Y) / std::max(Node.RadiusV * 2.2f, 0.04f);
				const float DistSq = DU * DU + DV * DV;
				if (DistSq < 1.0f)
				{
					Factor = std::max(Factor, 1.0f - std::sqrt(DistSq));
				}
			}
			WeldedTearFactor[w] = Factor;
		}
	}

	bTearMaskDirty = false;
	TearMaskUploadCooldown = 0.066f;
}

void UStockingComponent::DestroyNvClothResources()
{
	if (!FClothCore::IsInitialized())
	{
		ClothInstance = nullptr;
		ClothFabric = nullptr;
		ClothSolver = nullptr;
		return;
	}

	if (ClothSolver && ClothInstance)
	{
		ClothBridge_RemoveClothFromSolver(ClothSolver, ClothInstance);
	}
	if (ClothInstance)
	{
		ClothBridge_DestroyCloth(ClothInstance);
		ClothInstance = nullptr;
	}
	if (ClothFabric)
	{
		ClothBridge_DestroyFabric(ClothFabric);
		ClothFabric = nullptr;
	}
	if (ClothSolver)
	{
		ClothBridge_DestroySolver(ClothSolver);
		ClothSolver = nullptr;
	}
}

void UStockingComponent::RebuildNvClothFromSimMesh()
{
	DestroyNvClothResources();

	if (IsInGameWorld(this))
	{
		return;
	}

	const int32 ParticleCount = SimBindPositions.Num();
	const int32 TriCount = SimTriangleIndices.Num() / 3;
	if (ParticleCount == 0 || TriCount == 0)
	{
		return;
	}

	if (!FClothCore::IsInitialized() && !FClothCore::Init())
	{
		return;
	}

	FClothFactoryHandle Factory = FClothCore::GetFactory();
	if (!Factory)
	{
		return;
	}

	TArray<float> InvMasses;
	InvMasses.SetNum(ParticleCount);
	ClothParticlesBuffer.SetNum(ParticleCount);
	ClothPrevParticlesBuffer.SetNum(ParticleCount);
	ClothMotionConstraintsBuffer.SetNum(ParticleCount);
	ClothSeparationConstraintsBuffer.SetNum(ParticleCount);

	for (int32 i = 0; i < ParticleCount; ++i)
	{
		InvMasses[i] = 1.0f;
		ClothParticlesBuffer[i] = { SimBindPositions[i].X, SimBindPositions[i].Y, SimBindPositions[i].Z, 1.0f };
		ClothPrevParticlesBuffer[i] = ClothParticlesBuffer[i];
	}

	FClothBridgeMeshDesc MeshDesc{};
	MeshDesc.Positions = &SimBindPositions[0].X;
	MeshDesc.PositionStride = sizeof(FVector);
	MeshDesc.VertexCount = static_cast<uint32_t>(ParticleCount);
	MeshDesc.InvMasses = InvMasses.GetData();
	MeshDesc.InvMassStride = sizeof(float);
	MeshDesc.Indices = SimTriangleIndices.GetData();
	MeshDesc.TriangleCount = static_cast<uint32_t>(TriCount);
	MeshDesc.CookGravity = { 0.0f, 0.0f, -1.0f };
	MeshDesc.bUseGeodesicTether = false;

	ClothFabric = ClothBridge_CookFabric(Factory, &MeshDesc);
	if (!ClothFabric)
	{
		return;
	}

	ClothInstance = ClothBridge_CreateCloth(Factory, ClothFabric, ClothParticlesBuffer.GetData(), static_cast<uint32_t>(ParticleCount));
	if (!ClothInstance)
	{
		DestroyNvClothResources();
		return;
	}

	ClothSolver = ClothBridge_CreateSolver(Factory);
	if (!ClothSolver)
	{
		DestroyNvClothResources();
		return;
	}

	ClothBridge_AddClothToSolver(ClothSolver, ClothInstance);
	const FClothBridgeVec3 ZeroPos{ 0.0f, 0.0f, 0.0f };
	const FClothBridgeQuat IdentityRot{ 0.0f, 0.0f, 0.0f, 1.0f };
	ClothBridge_SetTransform(ClothInstance, &ZeroPos, &IdentityRot, true);

	ApplyNvClothParams();
	bNeedsClothTeleport = true;
}

bool UStockingComponent::StepNvClothJointWrinkles(
	const TArray<FVector>& SkinnedPos,
	const TArray<FVector>& SkinnedNormal,
	const FVector& AnchorNormal,
	TArray<FVector>& OutSimPos,
	TArray<float>& OutCompression)
{
	if (IsInGameWorld(this))
	{
		return false;
	}

	const int32 WeldedCount = static_cast<int32>(WeldedVertexCount);
	if (!bUseNvClothSimulation || !ClothSolver || !ClothInstance || !ClothFabric ||
		WeldedCount == 0 || SkinnedPos.Num() != WeldedCount || SimBindPositions.Num() != WeldedCount)
	{
		return false;
	}

	// 동일 프레임 중복 호출 시 직전 솔브 결과를 즉시 재사용
	if (bClothSteppedThisFrame && CachedClothSimPos.Num() == WeldedCount && CachedClothCompression.Num() == WeldedCount)
	{
		OutSimPos = CachedClothSimPos;
		OutCompression = CachedClothCompression;
		return bHasActiveClothWrinkles;
	}
	bPullTargetDirty = false;

	if (bNvClothParamsDirty)
	{
		ApplyNvClothParams();
	}

	// 관절 굽힘에 따른 방향성 및 평균 에지 압축률을 계산
	ScratchRawCompression.SetNum(WeldedCount, false);
	float PeakComp = 0.0f;
	for (int32 w = 0; w < WeldedCount; ++w)
	{
		float CompSum = 0.0f;
		float MaxComp = 0.0f;
		const TArray<FNeighborEdge>& Edges = WeldedAdjacency[w];
		for (int32 e = 0; e < Edges.Num(); ++e)
		{
			const float CurLen = (SkinnedPos[w] - SkinnedPos[static_cast<int32>(Edges[e].NeighborVertex)]).Size();
			const float Ratio = CurLen / Edges[e].RestLength;
			if (Ratio < 1.0f)
			{
				const float Comp = 1.0f - Ratio;
				CompSum += Comp;
				MaxComp = std::max(MaxComp, Comp);
			}
		}
		const float AvgComp = Edges.IsEmpty() ? 0.0f : (CompSum / static_cast<float>(Edges.Num()));
		ScratchRawCompression[w] = 0.40f * AvgComp + 0.60f * MaxComp;
		PeakComp = std::max(PeakComp, ScratchRawCompression[w]);
	}

	// 관절이 펴진 상태이고 활성 주름이 없으면 솔버 스텝을 생략해 프레임 확보
	if (!bNeedsClothTeleport && PeakComp < 0.015f && !bHasActiveClothWrinkles)
	{
		bClothAwakeLastFrame = false;
		bClothSteppedThisFrame = true;
		return false;
	}

	// 휴면 상태에서 깨어날 때 현재 스키닝 포즈로 파티클을 정렬해 튐 방지
	if (!bClothAwakeLastFrame)
	{
		bNeedsClothTeleport = true;
	}

	// 압축 영역 경계를 인접 정점으로 한 차례 부드럽게 확산
	OutCompression.SetNum(WeldedCount, false);
	for (int32 w = 0; w < WeldedCount; ++w)
	{
		float Sum = ScratchRawCompression[w] * 2.0f;
		float Weight = 2.0f;
		const TArray<FNeighborEdge>& Edges = WeldedAdjacency[w];
		for (int32 e = 0; e < Edges.Num(); ++e)
		{
			Sum += ScratchRawCompression[static_cast<int32>(Edges[e].NeighborVertex)];
			Weight += 1.0f;
		}
		OutCompression[w] = Sum / Weight;
	}

	(void)AnchorNormal;
	const float BackstopR = std::max(ClothBackstopRadius, 0.008f * ReferenceScale);
	const float BaseSlack = 0.0002f * ReferenceScale;
	const float SafeSlack = std::max(ClothJointWrinkleSlack, 0.0f);
	const float SafeScale = std::max(ReferenceScale, 0.05f);
	const float FoldFreqZ = 140.0f / SafeScale;
	const float FoldCurveX = 18.0f / SafeScale;
	const float FoldCurveY = 14.0f / SafeScale;
	const float MaxTangentDrift = 0.0012f * SafeScale;

	const float DynamicSurfaceOffset = SurfaceOffset - BakedSurfaceOffset;
	bool bAnyCompressed = false;
	for (int32 w = 0; w < WeldedCount; ++w)
	{
		const FVector& P = SkinnedPos[w];
		const FVector& N = SkinnedNormal[w];

		float CompFactor = 0.0f;
		float CreaseLift = 0.0f;
		float RidgeTargetH = 0.0f;
		float AllowedRadius = 0.0f;

		if (WeldedEdgeMasks[w] >= 0.22f)
		{
			CompFactor = FMath::Clamp((OutCompression[w] - 0.015f) / 0.18f, 0.0f, 1.0f);
			if (CompFactor > 0.01f)
			{
				bAnyCompressed = true;
				// 오목 관절 접힘부에서 곡면 피부가 뚫리지 않도록 미세한 기본 띄움과 타이트한 아코디언 능선 계산
				CreaseLift = CompFactor * (0.0022f * SafeScale);
				const FVector& BindP = SimBindPositions[w];
				const float CurvedZ = BindP.Z + (0.006f * SafeScale) * std::sin(BindP.X * FoldCurveX + BindP.Y * FoldCurveY);
				const float PrimaryFold = 0.5f + 0.5f * std::sin(CurvedZ * FoldFreqZ);
				const float SecondaryFold = 0.5f + 0.5f * std::sin(CurvedZ * FoldFreqZ * 1.9f + 1.2f);
				const float FoldProfile = std::pow(PrimaryFold, 1.2f) * 0.75f + SecondaryFold * 0.25f;
				RidgeTargetH = CompFactor * SafeSlack * FoldProfile;
			}
			AllowedRadius = BaseSlack + CompFactor * SafeSlack * 0.35f;
		}

		const FVector SkinTarget = P + N * (DynamicSurfaceOffset + CreaseLift);

		// 피부 밀착 백스톱 구와 타이트한 모션 제약 구로 붕뜸과 관통을 동시에 방지
		const FVector BackstopCenter = P - N * BackstopR;
		const float BackstopClearance = BackstopR + DynamicSurfaceOffset + CreaseLift + RidgeTargetH * 0.65f;
		ClothSeparationConstraintsBuffer[w] = { BackstopCenter.X, BackstopCenter.Y, BackstopCenter.Z, BackstopClearance };

		const FVector MotionCenter = SkinTarget + N * RidgeTargetH;
		ClothMotionConstraintsBuffer[w] = { MotionCenter.X, MotionCenter.Y, MotionCenter.Z, AllowedRadius };
	}

	ClothBridge_SetSeparationConstraints(ClothInstance, ClothSeparationConstraintsBuffer.GetData(), static_cast<uint32_t>(WeldedCount));
	ClothBridge_SetMotionConstraints(ClothInstance, ClothMotionConstraintsBuffer.GetData(), static_cast<uint32_t>(WeldedCount));

	if (bNeedsClothTeleport)
	{
		for (int32 w = 0; w < WeldedCount; ++w)
		{
			const FVector SkinTarget = SkinnedPos[w] + SkinnedNormal[w] * DynamicSurfaceOffset;
			ClothParticlesBuffer[w] = { SkinTarget.X, SkinTarget.Y, SkinTarget.Z, 1.0f };
		}
		ClothBridge_WriteParticles(ClothInstance, ClothParticlesBuffer.GetData(), static_cast<uint32_t>(WeldedCount));
		ClothBridge_ClearInterpolation(ClothInstance);
		bNeedsClothTeleport = false;
	}

	const float StepDt = std::clamp(PendingClothDeltaTime, 1.0f / 120.0f, 1.0f / 30.0f);
	ClothBridge_StepSolver(ClothSolver, StepDt);
	ClothBridge_ReadParticles(ClothInstance, ClothParticlesBuffer.GetData(), static_cast<uint32_t>(WeldedCount));

	OutSimPos.SetNum(WeldedCount);
	float MaxOutwardOffset = 0.0f;
	for (int32 w = 0; w < WeldedCount; ++w)
	{
		const float CompFactor = (WeldedEdgeMasks[w] >= 0.22f)
			? FMath::Clamp((OutCompression[w] - 0.015f) / 0.18f, 0.0f, 1.0f)
			: 0.0f;
		const float CreaseLift = CompFactor * (0.0022f * SafeScale);
		const FVector SkinTarget = SkinnedPos[w] + SkinnedNormal[w] * (DynamicSurfaceOffset + CreaseLift);
		const FVector& N = SkinnedNormal[w];
		FVector SimP(ClothParticlesBuffer[w].X, ClothParticlesBuffer[w].Y, ClothParticlesBuffer[w].Z);
		if (!std::isfinite(SimP.X) || !std::isfinite(SimP.Y) || !std::isfinite(SimP.Z))
		{
			SimP = SkinTarget;
		}
		else
		{
			// 법선 바깥 여유와 접선 미끄러짐 한계를 동시에 제한해 오목 관절 피부 노출 차단
			const FVector Rel = SimP - SkinTarget;
			const float RawNormalDist = Rel.Dot(N);
			const float MaxOutH = BaseSlack + CompFactor * SafeSlack * 1.15f;
			const float ClampedNormalDist = std::clamp(RawNormalDist, 0.0f, MaxOutH);
			FVector TangentRel = Rel - N * RawNormalDist;
			const float TanLenSq = TangentRel.SizeSquared();
			const float LocalTanLimit = (WeldedEdgeMasks[w] >= 0.22f) ? (MaxTangentDrift * (0.25f + 0.75f * CompFactor)) : 0.0f;
			if (TanLenSq > LocalTanLimit * LocalTanLimit && TanLenSq > 1e-12f)
			{
				TangentRel = TangentRel * (LocalTanLimit / std::sqrt(TanLenSq));
			}
			SimP = SkinTarget + N * ClampedNormalDist + TangentRel;
			MaxOutwardOffset = std::max(MaxOutwardOffset, ClampedNormalDist + CreaseLift);
		}
		OutSimPos[w] = SimP;
	}

	bHasActiveClothWrinkles = bAnyCompressed || (MaxOutwardOffset > 0.001f * SafeScale);
	bClothAwakeLastFrame = true;
	CachedClothSimPos = OutSimPos;
	CachedClothCompression = OutCompression;
	bClothSteppedThisFrame = true;
	return true;
}

void UStockingComponent::ComputeGrabGeodesicDistances()
{
	GrabGeodesicDistances.SetNum(static_cast<int32>(WeldedVertexCount));
	for (uint32 i = 0; i < WeldedVertexCount; ++i)
	{
		GrabGeodesicDistances[static_cast<int32>(i)] = FLT_MAX;
	}

	const USkeletalMesh* Mesh = GetSkeletalMesh();
	if (!Mesh || Mesh->GetRenderData().LODRenderData.IsEmpty() || WeldedVertexCount == 0)
	{
		return;
	}

	const FSkeletalMeshLODRenderData& LOD = Mesh->GetRenderData().LODRenderData[0];
	const int32 TriCount = LOD.Indices.Num() / 3;
	if (static_cast<int32>(GrabbedTriangleIndex) >= TriCount)
	{
		return;
	}

	const uint32 I0 = LOD.Indices[GrabbedTriangleIndex * 3];
	const uint32 I1 = LOD.Indices[GrabbedTriangleIndex * 3 + 1];
	const uint32 I2 = LOD.Indices[GrabbedTriangleIndex * 3 + 2];

	const FVector AnchorBindPos =
		LOD.BindVertices[I0].Position * GrabBarycentric.X +
		LOD.BindVertices[I1].Position * GrabBarycentric.Y +
		LOD.BindVertices[I2].Position * GrabBarycentric.Z;

	using FDistNode = std::pair<float, uint32>;
	std::priority_queue<FDistNode, std::vector<FDistNode>, std::greater<FDistNode>> Queue;

	const uint32 SeedVerts[3] = { I0, I1, I2 };
	for (int32 s = 0; s < 3; ++s)
	{
		const uint32 Wid = WeldedVertexIds[SeedVerts[s]];
		const float InitDist = (LOD.BindVertices[SeedVerts[s]].Position - AnchorBindPos).Size();
		if (InitDist < GrabGeodesicDistances[static_cast<int32>(Wid)])
		{
			GrabGeodesicDistances[static_cast<int32>(Wid)] = InitDist;
			Queue.push({ InitDist, Wid });
		}
	}

	// 잡은 삼각형에서 메시 표면을 따라 최단 거리 전파
	while (!Queue.empty())
	{
		const FDistNode Top = Queue.top();
		Queue.pop();

		const float CurDist = Top.first;
		const uint32 CurW = Top.second;
		if (CurDist > GrabGeodesicDistances[static_cast<int32>(CurW)])
		{
			continue;
		}

		const TArray<FNeighborEdge>& Edges = WeldedAdjacency[static_cast<int32>(CurW)];
		for (int32 e = 0; e < Edges.Num(); ++e)
		{
			const uint32 NextW = Edges[e].NeighborVertex;
			const float NextDist = CurDist + Edges[e].RestLength;
			if (NextDist < GrabGeodesicDistances[static_cast<int32>(NextW)])
			{
				GrabGeodesicDistances[static_cast<int32>(NextW)] = NextDist;
				Queue.push({ NextDist, NextW });
			}
		}
	}
}

void UStockingComponent::StepPullState(float DeltaTime)
{
	if (DeltaTime <= 0.0f || !std::isfinite(DeltaTime))
	{
		return;
	}

	if (!bIsPulling && (CurrentPullOffsetLocal.SizeSquared() > 1e-10f || PullVelocityLocal.SizeSquared() > 1e-10f))
	{
		// 감쇠 탄성 스프링으로 놓았을 때 피부에 착 붙는 스냅백 복원
		const float SafeDt = std::min(DeltaTime, 1.0f / 30.0f);
		const float Stiffness = std::max(SpringStiffness, 10.0f);
		const float Damping = std::max(SpringDamping, 1.0f);

		const FVector Accel = CurrentPullOffsetLocal * (-Stiffness) - PullVelocityLocal * Damping;
		PullVelocityLocal += Accel * SafeDt;
		CurrentPullOffsetLocal += PullVelocityLocal * SafeDt;

		// 피부 안쪽으로 과도하게 파고드는 음의 반동 억제
		const float ScaleSq = ReferenceScale * ReferenceScale;
		if (CurrentPullOffsetLocal.SizeSquared() <= 1e-7f * ScaleSq && PullVelocityLocal.SizeSquared() <= 1e-5f * ScaleSq)
		{
			CurrentPullOffsetLocal = FVector::ZeroVector;
			PullVelocityLocal = FVector::ZeroVector;
			bHasGrabAnchor = false;
		}
		MarkBoneTransformsDirty();
	}
}

void UStockingComponent::ApplyStockingDeformation(TArray<FVertex>& InOutVertices, FBox& InOutBounds)
{
	USkeletalMesh* Mesh = GetSkeletalMesh();
	if (!Mesh || Mesh->GetRenderData().LODRenderData.IsEmpty() || InOutVertices.IsEmpty() || WeldedVertexCount == 0)
	{
		return;
	}

	const FSkeletalMeshLODRenderData& LOD = Mesh->GetRenderData().LODRenderData[0];
	const int32 VertexCount = InOutVertices.Num();
	const int32 TriCount = LOD.Indices.Num() / 3;
	const int32 WeldedCount = static_cast<int32>(WeldedVertexCount);

	// 심 정점이 같은 위치와 노멀을 공유하도록 용접 정점 기준 평균 위치와 노멀 계산
	ScratchWeldedPos.SetNum(WeldedCount, false);
	ScratchWeldedNormal.SetNum(WeldedCount, false);
	ScratchWeldedCounts.SetNum(WeldedCount, false);
	for (int32 w = 0; w < WeldedCount; ++w)
	{
		ScratchWeldedPos[w] = FVector::ZeroVector;
		ScratchWeldedNormal[w] = FVector::ZeroVector;
		ScratchWeldedCounts[w] = 0;
	}

	for (int32 v = 0; v < VertexCount; ++v)
	{
		const int32 W = static_cast<int32>(WeldedVertexIds[v]);
		ScratchWeldedPos[W] += InOutVertices[v].Position;
		ScratchWeldedNormal[W] += InOutVertices[v].Normal;
		ScratchWeldedCounts[W] += 1;
	}

	for (int32 w = 0; w < WeldedCount; ++w)
	{
		if (ScratchWeldedCounts[w] > 0)
		{
			ScratchWeldedPos[w] = ScratchWeldedPos[w] / static_cast<float>(ScratchWeldedCounts[w]);
		}
		if (ScratchWeldedNormal[w].SizeSquared() > 1e-10f)
		{
			ScratchWeldedNormal[w] = ScratchWeldedNormal[w].Normalized();
		}
		else
		{
			ScratchWeldedNormal[w] = FVector(0.0f, 0.0f, 1.0f);
		}
	}

	// 잡은 삼각형의 현재 스키닝 위치 기준으로 앵커와 당김 오프셋 계산
	FVector AnchorNormal(0.0f, 0.0f, 1.0f);
	if (bHasGrabAnchor && static_cast<int32>(GrabbedTriangleIndex) < TriCount)
	{
		const int32 W0 = static_cast<int32>(WeldedVertexIds[LOD.Indices[GrabbedTriangleIndex * 3]]);
		const int32 W1 = static_cast<int32>(WeldedVertexIds[LOD.Indices[GrabbedTriangleIndex * 3 + 1]]);
		const int32 W2 = static_cast<int32>(WeldedVertexIds[LOD.Indices[GrabbedTriangleIndex * 3 + 2]]);

		LastAnchorLocal =
			ScratchWeldedPos[W0] * GrabBarycentric.X +
			ScratchWeldedPos[W1] * GrabBarycentric.Y +
			ScratchWeldedPos[W2] * GrabBarycentric.Z;

		AnchorNormal =
			ScratchWeldedNormal[W0] * GrabBarycentric.X +
			ScratchWeldedNormal[W1] * GrabBarycentric.Y +
			ScratchWeldedNormal[W2] * GrabBarycentric.Z;
		if (AnchorNormal.SizeSquared() > 1e-10f)
		{
			AnchorNormal = AnchorNormal.Normalized();
		}

		if (bIsPulling)
		{
			const FVector TargetLocal = GetWorldMatrix().Inverse().TransformPosition(PullTargetWorld);
			FVector DesiredOffset = TargetLocal - LastAnchorLocal;

			// 피부 안쪽으로 깊게 파고드는 성분 제거
			const float InwardDot = DesiredOffset.Dot(AnchorNormal);
			if (InwardDot < 0.0f)
			{
				DesiredOffset -= AnchorNormal * InwardDot;
			}

			// 정면 화면 드래그 시에도 피부에서 최소한 들리면서 원래 마우스 방향은 유지
			const float RawLen = DesiredOffset.Size();
			const float SafeMaxPull = std::max(MaxPullDistance, 0.001f);
			if (RawLen > 1e-6f)
			{
				const float CurrentOutward = DesiredOffset.Dot(AnchorNormal);
				const float MinOutwardLift = RawLen * 0.18f;
				if (CurrentOutward < MinOutwardLift)
				{
					DesiredOffset += AnchorNormal * (MinOutwardLift - CurrentOutward);
				}

				// 방향을 왜곡하지 않는 등방성 점근 탄성 저항으로 최대 신장 제한
				const float AdjustedLen = DesiredOffset.Size();
				const float SmoothLen = SafeMaxPull * (1.0f - std::exp(-AdjustedLen / (SafeMaxPull * 0.90f)));
				DesiredOffset = DesiredOffset * (SmoothLen / std::max(AdjustedLen, 1e-6f));
			}
			CurrentPullOffsetLocal = DesiredOffset;
		}
	}

	const float PullMagSq = CurrentPullOffsetLocal.SizeSquared();
	const bool bApplyPull = bHasGrabAnchor && PullMagSq > 1e-10f && GrabGeodesicDistances.Num() == WeldedCount;
	const float PullMag = bApplyPull ? std::sqrt(PullMagSq) : 0.0f;
	const float SafeRadius = std::max(PullRadius, 0.005f);
	const float SafeExponent = std::max(PullFalloffExponent, 0.25f);
	const float PullStrength = bApplyPull ? std::clamp(PullMag / std::max(MaxPullDistance, 0.01f), 0.0f, 1.0f) : 0.0f;
	MaxAppliedDisplacement = 0.0f;

	// 방사형 장력 주름 계산용 앵커 접평면 기저 생성
	const FVector RefUp = (std::fabs(AnchorNormal.Z) < 0.9f) ? FVector(0.0f, 0.0f, 1.0f) : FVector(1.0f, 0.0f, 0.0f);
	const FVector AnchorTangentU = FVector::Cross(RefUp, AnchorNormal).Normalized();
	const FVector AnchorTangentV = FVector::Cross(AnchorNormal, AnchorTangentU).Normalized();

	// 관절 접힘부 물리 주름을 NvCloth 백스톱과 모션 제약으로 시뮬레이션
	const bool bUsedNvCloth = StepNvClothJointWrinkles(ScratchWeldedPos, ScratchWeldedNormal, AnchorNormal, ScratchNvSimPos, ScratchNvCompression);

	ScratchDeformedWeldedPos.SetNum(WeldedCount, false);
	ScratchWeldedCrease.SetNum(WeldedCount, false);
	ScratchOutwardBulge.SetNum(WeldedCount, false);

	const float DynamicSurfaceOffset = SurfaceOffset - BakedSurfaceOffset;
	for (int32 w = 0; w < WeldedCount; ++w)
	{
		const FVector& BasePos = ScratchWeldedPos[w];
		const FVector& SmoothN = ScratchWeldedNormal[w];
		const FVector SkinSurfacePos = BasePos + SmoothN * DynamicSurfaceOffset;
		FVector NewPos = bUsedNvCloth ? ScratchNvSimPos[w] : SkinSurfacePos;
		ScratchOutwardBulge[w] = bUsedNvCloth ? (NewPos - SkinSurfacePos).Dot(SmoothN) : 0.0f;
		if (bUsedNvCloth)
		{
			MaxAppliedDisplacement = FMath::Max(MaxAppliedDisplacement, (NewPos - SkinSurfacePos).Size());
		}
		float CreaseSignal = 0.0f;

		if (bApplyPull)
		{
			const float SurfaceDist = GrabGeodesicDistances[w];
			if (SurfaceDist < SafeRadius)
			{
				const float NormR = SurfaceDist / SafeRadius;
				const float InvR = 1.0f - NormR;
				// 오목한 텐트형 핀치 프로파일로 손끝은 뾰족하게 주변은 부드럽게 당김
				const float ConcaveProfile = std::pow(InvR, SafeExponent) * (0.65f + 0.35f * InvR * InvR);

				// 당김 방향이 피부 바깥쪽을 향하는 면만 변형해 측면 관통 방지
				// 당김 정점이 측면 실루엣으로 밀리지 않고 정확히 피킹된 앵커에 고정되도록 앵커 기준 가중치 사용
				const float RawAnchorFacing = std::clamp(SmoothN.Dot(AnchorNormal), 0.0f, 1.0f);
				const float AnchorFacing = RawAnchorFacing * RawAnchorFacing;
				const float TearYieldBoost = (w < WeldedTearFactor.Num()) ? (1.0f + 0.18f * WeldedTearFactor[w]) : 1.0f;
				const float Weight = std::min(ConcaveProfile * AnchorFacing * TearYieldBoost, 1.0f);

				if (Weight > 1e-5f)
				{
					// 앵커 주변 정점은 마우스 당김 방향을 그대로 따르게 유지
					FVector VertexDelta = CurrentPullOffsetLocal * Weight;
					const float RawDeltaN = VertexDelta.Dot(SmoothN);
					if (RawDeltaN < 0.0f)
					{
						// 피부 안쪽으로 밀리는 성분 제거 및 반대편 측면 미끄러짐 감쇠
						const float RawDeltaLen = VertexDelta.Size();
						const float SideSafe = (RawDeltaLen > 1e-6f)
							? std::clamp(1.0f + RawDeltaN / RawDeltaLen, 0.0f, 1.0f)
							: 0.0f;
						VertexDelta = (VertexDelta - SmoothN * RawDeltaN) * (AnchorFacing * SideSafe);
					}

					// 측면 실루엣 부근에서만 곡면 현 보정으로 허벅지 옆 피부 노출 차단
					// 당김 주변 곡면 전 구간에서 접선 이동에 따른 현 관통을 최소 법선 띄움으로 차단
					float FinalDeltaN = VertexDelta.Dot(SmoothN);
					const FVector SideTan = VertexDelta - SmoothN * FinalDeltaN;
					const float TanLen = SideTan.Size();
					const float SafeLimbRadius = std::max(0.055f * ReferenceScale, 0.01f);
					const float SkirtMask = std::clamp(1.0f - ConcaveProfile * 0.85f, 0.0f, 1.0f);
					const float CurvatureLift = SkirtMask * ((TanLen * TanLen) / (1.2f * SafeLimbRadius) + TanLen * 0.18f)
						+ Weight * (0.003f * ReferenceScale);
					if (FinalDeltaN < CurvatureLift)
					{
						VertexDelta += SmoothN * (CurvatureLift - FinalDeltaN);
						FinalDeltaN = CurvatureLift;
					}

					// 잡은 지점에서 방사형으로 퍼지는 장력 주름 능선과 골 생성
					if (WrinkleAmplitude > 1e-5f && NormR > 0.03f)
					{
						const FVector Rel = BasePos - LastAnchorLocal;
						const float U = Rel.Dot(AnchorTangentU);
						const float V = Rel.Dot(AnchorTangentV);
						const float Angle = std::atan2(V, U);
						const float RingEnvelope = std::sin(NormR * 3.14159265f) * InvR;
						const float PrimaryWave = std::sin(Angle * WrinkleFrequency + NormR * 2.2f);
						const float SecondaryWave = 0.45f * std::sin(Angle * WrinkleFrequency * 2.0f - NormR * 3.5f + 1.1f);
						const float Wave = std::clamp((PrimaryWave + SecondaryWave) / 1.45f, -1.0f, 1.0f);
						const float WrinkleScale = PullStrength * RingEnvelope * AnchorFacing;

						// 능선은 바깥으로 솟고 골은 피부선 위까지만 낮춰 관통 방지
						const float RippleMag = (0.5f + 0.5f * Wave) * WrinkleAmplitude * WrinkleScale;
						VertexDelta += SmoothN * RippleMag;
						CreaseSignal = Wave * WrinkleScale;
					}

					NewPos += VertexDelta;
					MaxAppliedDisplacement = FMath::Max(MaxAppliedDisplacement, VertexDelta.Size());
				}
			}
		}

		ScratchDeformedWeldedPos[w] = NewPos;
		ScratchWeldedCrease[w] = CreaseSignal;
	}

	// 용접 정점 위치를 원본 정점에 일괄 반영해 심 갈라짐 방지
	for (int32 v = 0; v < VertexCount; ++v)
	{
		InOutVertices[v].Position = ScratchDeformedWeldedPos[static_cast<int32>(WeldedVertexIds[v])];
	}

	if (bUsedNvCloth || bApplyPull)
	{
		RecomputeDeformedNormals(InOutVertices);
	}

	// 인접 용접 정점 간 원래 길이 대비 늘어난 비율과 물리 주름 곡률을 정점 컬러에 기록
	ScratchWeldedStretch.SetNum(WeldedCount, false);
	MaxCurrentStretchRatio = 1.0f;
	const float CurvatureNorm = std::max(ClothJointWrinkleSlack * 0.20f, 0.002f * ReferenceScale);

	for (int32 w = 0; w < WeldedCount; ++w)
	{
		float RawRatio = 1.0f;
		float NeighborAvgBulge = ScratchOutwardBulge[w];
		if (!WeldedAdjacency[w].IsEmpty())
		{
			const TArray<FNeighborEdge>& Edges = WeldedAdjacency[w];
			float RatioSum = 0.0f;
			float BulgeSum = 0.0f;
			for (int32 e = 0; e < Edges.Num(); ++e)
			{
				const FNeighborEdge& Edge = Edges[e];
				const int32 Nw = static_cast<int32>(Edge.NeighborVertex);
				const float CurrentLen = (ScratchDeformedWeldedPos[w] - ScratchDeformedWeldedPos[Nw]).Size();
				RatioSum += CurrentLen / Edge.RestLength;
				BulgeSum += ScratchOutwardBulge[Nw];
			}
			const float InvCount = 1.0f / static_cast<float>(Edges.Num());
			RawRatio = RatioSum * InvCount;
			NeighborAvgBulge = BulgeSum * InvCount;
		}

		const float StretchRatio = std::clamp(RawRatio, 1.0f, 3.5f);
		// NvCloth 물리 곡률과 실제 포즈 압축률을 합성해 매크로 주름과 픽셀 미세 주름 신호 전달
		const float ClothCurvature = bUsedNvCloth
			? std::clamp((ScratchOutwardBulge[w] - NeighborAvgBulge) / CurvatureNorm, -1.0f, 1.0f)
			: 0.0f;
		const float PoseComp = bUsedNvCloth
			? std::clamp((ScratchNvCompression[w] - 0.012f) * 4.5f, 0.0f, 1.0f)
			: std::clamp((1.0f - RawRatio) * 3.5f, 0.0f, 1.0f);
		// 압축 관절에서는 음수 압축 신호를 유지해 픽셀 미세 주름이 곡률에 상쇄되지 않게 보존
		const float JointSignal = (PoseComp > 0.05f)
			? (-PoseComp * 0.95f)
			: (ClothCurvature * 0.35f);
		const float CombinedSignal = std::clamp(ScratchWeldedCrease[w] + JointSignal, -1.0f, 1.0f);
		ScratchWeldedCrease[w] = std::clamp(0.5f + 0.5f * CombinedSignal, 0.0f, 1.0f);

		ScratchWeldedStretch[w] = StretchRatio;
		MaxCurrentStretchRatio = FMath::Max(MaxCurrentStretchRatio, StretchRatio);
	}

	UpdatePullTearing(LOD, ScratchDeformedWeldedPos, ScratchWeldedStretch);
	if (bTearMaskDirty && TearMaskUploadCooldown <= 0.0f)
	{
		RebuildTearMaskTexture();
	}

	FVector MinP(FLT_MAX, FLT_MAX, FLT_MAX);
	FVector MaxP(-FLT_MAX, -FLT_MAX, -FLT_MAX);
	for (int32 v = 0; v < VertexCount; ++v)
	{
		if (GetDebugWeightBone() < 0)
		{
			const int32 W = static_cast<int32>(WeldedVertexIds[v]);
			InOutVertices[v].Color.Z = ScratchWeldedCrease[W];
			InOutVertices[v].Color.W = ScratchWeldedStretch[W];
		}

		const FVector& P = InOutVertices[v].Position;
		MinP.X = FMath::Min(MinP.X, P.X);
		MinP.Y = FMath::Min(MinP.Y, P.Y);
		MinP.Z = FMath::Min(MinP.Z, P.Z);
		MaxP.X = FMath::Max(MaxP.X, P.X);
		MaxP.Y = FMath::Max(MaxP.Y, P.Y);
		MaxP.Z = FMath::Max(MaxP.Z, P.Z);
	}

	const FVector Pad(BoundsPadding, BoundsPadding, BoundsPadding);
	InOutBounds = FBox(MinP - Pad, MaxP + Pad);
}

void UStockingComponent::RecomputeDeformedNormals(TArray<FVertex>& InOutVertices) const
{
	const USkeletalMesh* Mesh = GetSkeletalMesh();
	if (!Mesh || Mesh->GetRenderData().LODRenderData.IsEmpty() || WeldedVertexCount == 0)
	{
		return;
	}

	const TArray<uint32>& Indices = Mesh->GetRenderData().LODRenderData[0].Indices;
	const int32 WeldedCount = static_cast<int32>(WeldedVertexCount);

	ScratchAccumNormals.SetNum(WeldedCount, false);
	for (int32 w = 0; w < WeldedCount; ++w)
	{
		ScratchAccumNormals[w] = FVector::ZeroVector;
	}

	for (int32 i = 0; i + 2 < Indices.Num(); i += 3)
	{
		const uint32 I0 = Indices[i];
		const uint32 I1 = Indices[i + 1];
		const uint32 I2 = Indices[i + 2];

		const FVector FaceNormal = FVector::Cross(
			InOutVertices[I1].Position - InOutVertices[I0].Position,
			InOutVertices[I2].Position - InOutVertices[I0].Position);
		ScratchAccumNormals[static_cast<int32>(WeldedVertexIds[I0])] += FaceNormal;
		ScratchAccumNormals[static_cast<int32>(WeldedVertexIds[I1])] += FaceNormal;
		ScratchAccumNormals[static_cast<int32>(WeldedVertexIds[I2])] += FaceNormal;
	}

	for (int32 w = 0; w < WeldedCount; ++w)
	{
		if (ScratchAccumNormals[w].SizeSquared() > 1e-12f)
		{
			ScratchAccumNormals[w] = ScratchAccumNormals[w].Normalized();
		}
	}

	const int32 VertexCount = InOutVertices.Num();
	for (int32 v = 0; v < VertexCount; ++v)
	{
		const FVector& N = ScratchAccumNormals[static_cast<int32>(WeldedVertexIds[v])];
		if (N.SizeSquared() > 1e-12f)
		{
			InOutVertices[v].Normal = N;
		}
	}
}

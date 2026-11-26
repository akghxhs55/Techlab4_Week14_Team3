#include "EnginePCH.h"
#include "GameFramework/Actor/StockingActor.h"
#include "Asset/AssetManager.h"
#include "Asset/AssetUtils.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StockingComponent.h"
#include "Components/StockingInteractionComponent.h"
#include "Engine/World.h"
#include "Render/SkeletalMesh.h"
#include "Render/StockingRegion.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"

#include <cctype>
#include <filesystem>

namespace
{
	FString ToLowerStockingAscii(const FString& Text)
	{
		FString Lower = Text;
		for (size_t i = 0; i < Lower.size(); ++i)
		{
			Lower[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(Lower[i])));
		}
		return Lower;
	}

	bool ContainsStockingKeyword(const FString& Text, const char* Keyword)
	{
		const FString LowerText = ToLowerStockingAscii(Text);
		const FString LowerKey = ToLowerStockingAscii(FString(Keyword));
		return LowerText.find(LowerKey) != FString::npos;
	}

	void CollectLegBoneIndices(const USkeletalMesh& Mesh, TArray<int32>& OutLegBones)
	{
		OutLegBones.Reset();
		const TArray<FBoneInfo>& Bones = Mesh.GetRefSkeleton();
		for (int32 i = 0; i < Bones.Num(); ++i)
		{
			if (ContainsStockingKeyword(Bones[i].Name, "thigh") ||
				ContainsStockingKeyword(Bones[i].Name, "calf") ||
				ContainsStockingKeyword(Bones[i].Name, "shin") ||
				ContainsStockingKeyword(Bones[i].Name, "knee") ||
				ContainsStockingKeyword(Bones[i].Name, "foot") ||
				ContainsStockingKeyword(Bones[i].Name, "leg"))
			{
				OutLegBones.Add(i);
			}
		}
	}
}

AStockingActor::AStockingActor()
{
	PrimaryActorTick.bCanEverTick = true;

	USkeletalMeshComponent* MeshComponent = GetSkeletalMeshComponent();
	if (MeshComponent)
	{
		MeshComponent->SetRelativeLocation(FVector(0.0f, 0.0f, -1.0f));
		MeshComponent->bStartRagdollOnBeginPlay = bRagdollOnBeginPlay;
		if (USkeletalMesh* FoundMesh = ResolveSourceMeshFromHint())
		{
			MeshComponent->SetSkeletalMesh(FoundMesh);
		}
		else
		{
			KRS_LOG(Warning, "AStockingActor: baked mesh matching '{}' not found in Content. Bake female_animation.fbx to .fbx.bin and generate a PhysicsAsset.", SourceMeshPathHint);
		}
	}

	StockingComponent = CreateDefaultSubobject<UStockingComponent>("StockingComponent");
	if (StockingComponent && MeshComponent)
	{
		StockingComponent->SetupAttachment(MeshComponent);
		StockingComponent->SetSourceSkeletalMeshComponent(MeshComponent);
	}

	StockingInteractionComponent = CreateDefaultSubobject<UStockingInteractionComponent>("StockingInteractionComponent");
	if (StockingInteractionComponent)
	{
		StockingInteractionComponent->SetTargetStockingComponent(StockingComponent);
		StockingInteractionComponent->bUseGameViewportInput = false;
	}

	ApplyStockingTearingSettings();
	RebuildStockingFromSourceMesh();
}

void AStockingActor::BeginPlay()
{
	USkeletalMeshComponent* MeshComponent = GetSkeletalMeshComponent();
	if (MeshComponent)
	{
		// 스타킹 충돌 리스너를 먼저 연결한 뒤 래그돌을 시작하도록 순서를 제어
		MeshComponent->bStartRagdollOnBeginPlay = false;
	}

	Super::BeginPlay();

	if (MeshComponent)
	{
		MeshComponent->bStartRagdollOnBeginPlay = bRagdollOnBeginPlay;
		if (!MeshComponent->GetSkeletalMesh())
		{
			if (USkeletalMesh* FoundMesh = ResolveSourceMeshFromHint())
			{
				MeshComponent->SetSkeletalMesh(FoundMesh);
			}
		}
	}

	bool bPreservedConfiguredStocking = false;
	if (StockingComponent)
	{
		if (!StockingComponent->GetSourceSkeletalMeshComponent() && MeshComponent)
		{
			StockingComponent->SetSourceSkeletalMeshComponent(MeshComponent);
		}
		if (UStockingRegion* CompRegion = StockingComponent->GetStockingRegion();
			CompRegion && CompRegion->GetSelectedTriangleCount() > 0)
		{
			StockingRegion = CompRegion;
			USkeletalMesh* BuiltMesh = StockingComponent->GetSkeletalMesh();
			const bool bHasValidBuiltMesh = BuiltMesh && !BuiltMesh->GetRenderData().LODRenderData.IsEmpty();
			if (bHasValidBuiltMesh)
			{
				BuiltSourceMesh = MeshComponent ? MeshComponent->GetSkeletalMesh() : nullptr;
				StockingComponent->SyncTearMaskBinding();
				bPreservedConfiguredStocking = true;
			}
			else if (MeshComponent && MeshComponent->GetSkeletalMesh())
			{
				FString BuildError;
				if (StockingComponent->RebuildStockingMesh(BuildError))
				{
					BuiltSourceMesh = MeshComponent->GetSkeletalMesh();
					if (StockingInteractionComponent)
					{
						StockingInteractionComponent->SetTargetStockingComponent(StockingComponent);
					}
					bPreservedConfiguredStocking = true;
				}
			}
		}
	}

	if (!bPreservedConfiguredStocking)
	{
		RebuildStockingFromSourceMesh();
		ApplyStockingTearingSettings();
	}

	UWorld* World = GetWorld();
	if (bRagdollOnBeginPlay && World && World->IsGameWorld())
	{
		bRagdollStartAttempted = true;
		if (!StartRagdoll())
		{
			KRS_LOG(Warning, "AStockingActor: StartRagdoll failed on BeginPlay (check PhysicsAsset on SkeletalMesh).");
		}
	}
}

void AStockingActor::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	USkeletalMeshComponent* MeshComponent = GetSkeletalMeshComponent();
	if (MeshComponent && MeshComponent->GetSkeletalMesh() != BuiltSourceMesh)
	{
		RebuildStockingFromSourceMesh();
		ApplyStockingTearingSettings();
		UWorld* World = GetWorld();
		if (bRagdollOnBeginPlay && !bRagdollStartAttempted && World && World->IsGameWorld() && !MeshComponent->IsRagdollActive())
		{
			bRagdollStartAttempted = true;
			StartRagdoll();
		}
	}
}

void AStockingActor::PostEditChangeProperty(const FProperty& Property)
{
	Super::PostEditChangeProperty(Property);

	USkeletalMeshComponent* MeshComponent = GetSkeletalMeshComponent();
	if (MeshComponent)
	{
		MeshComponent->bStartRagdollOnBeginPlay = bRagdollOnBeginPlay;
	}

	const FString& Name = Property.Name;
	if (Name == "SourceMeshPathHint")
	{
		if (MeshComponent)
		{
			if (USkeletalMesh* FoundMesh = ResolveSourceMeshFromHint())
			{
				MeshComponent->SetSkeletalMesh(FoundMesh);
			}
		}
		RebuildStockingFromSourceMesh();
	}
	else if (Name == "StockingRegionPath" || Name == "AutoLegMaxRadiusRatio" || Name == "AutoLegMaxHeightRatio")
	{
		StockingRegion = nullptr;
		if (StockingComponent)
		{
			StockingComponent->SetStockingRegion(nullptr);
		}
		RebuildStockingFromSourceMesh();
	}

	ApplyStockingTearingSettings();
}

USkeletalMesh* AStockingActor::ResolveSourceMeshFromHint() const
{
	if (SourceMeshPathHint.empty())
	{
		return nullptr;
	}

	if (USkeletalMesh* DirectMesh = UAssetManager::GetAssetByPath<USkeletalMesh>(SourceMeshPathHint))
	{
		return DirectMesh;
	}

	const FString LowerHint = ToLowerStockingAscii(SourceMeshPathHint);
	for (TObjectIterator<USkeletalMesh> It; It; ++It)
	{
		USkeletalMesh* Candidate = *It;
		if (!Candidate)
		{
			continue;
		}
		if (ToLowerStockingAscii(Candidate->GetPath()).find(LowerHint) != FString::npos)
		{
			return Candidate;
		}
	}

	const TArray<FString> UnloadedMeshes = UAssetManager::GetUnloadedAssetPaths(EAssetType::SkeletalMesh);
	for (int32 i = 0; i < UnloadedMeshes.Num(); ++i)
	{
		if (ToLowerStockingAscii(UnloadedMeshes[i]).find(LowerHint) != FString::npos)
		{
			return UAssetManager::GetAssetByPath<USkeletalMesh>(UnloadedMeshes[i]);
		}
	}

	return nullptr;
}

void AStockingActor::ApplyStockingTearingSettings()
{
	if (!StockingComponent)
	{
		return;
	}

	StockingComponent->bEnableTearing = true;
	StockingComponent->bEnableRagdollImpactTearing = bEnableImpactTearing;
	StockingComponent->ImpactTearImpulseThreshold = ImpactTearImpulseThreshold;
	StockingComponent->ImpactScrapeSensitivity = ImpactScrapeSensitivity;
	StockingComponent->ImpactTearCooldownDuration = ImpactTearCooldownDuration;
}

bool AStockingActor::RebuildStockingFromSourceMesh()
{
	USkeletalMeshComponent* MeshComponent = GetSkeletalMeshComponent();
	USkeletalMesh* SourceMesh = MeshComponent ? MeshComponent->GetSkeletalMesh() : nullptr;
	if (!MeshComponent || !SourceMesh || !StockingComponent)
	{
		BuiltSourceMesh = nullptr;
		return false;
	}

	if (!StockingRegion && StockingComponent->GetStockingRegion())
	{
		StockingRegion = StockingComponent->GetStockingRegion();
	}
	if (!StockingRegion)
	{
		StockingRegion = NewObject<UStockingRegion>(this);
	}
	if (!StockingRegion)
	{
		return false;
	}

	const bool bHasPreloadedRegion = StockingRegionPath.empty() && StockingRegion->GetSelectedTriangleCount() > 0;
	bool bLoadedRegion = bHasPreloadedRegion;
	if (!bHasPreloadedRegion)
	{
		StockingRegion->BindToSourceMesh(*SourceMesh, SourceMesh->GetPath(), 0);
		StockingRegion->SubdivisionIterations = 1;
	}

	if (!bLoadedRegion && !StockingRegionPath.empty())
	{
		FString ResolvedRegionPath = StockingRegionPath;
		if (!std::filesystem::exists(ResolvedRegionPath))
		{
			const FString AssetFile = FAssetUtils::ResolveAssetFilePath(StockingRegionPath);
			if (!AssetFile.empty())
			{
				ResolvedRegionPath = AssetFile;
			}
		}
		if (std::filesystem::exists(ResolvedRegionPath))
		{
			FString LoadError;
			bLoadedRegion = StockingRegion->LoadFromFile(ResolvedRegionPath, LoadError);
		}
	}

	if (!bLoadedRegion)
	{
		TArray<int32> LegBones;
		CollectLegBoneIndices(*SourceMesh, LegBones);
		StockingRegion->ClearTriangles();
		if (!LegBones.IsEmpty())
		{
			StockingRegion->SelectTrianglesByBoneFilter(
				*SourceMesh,
				LegBones,
				0.25f,
				AutoLegMaxRadiusRatio,
				AutoLegMaxHeightRatio);
		}
		if (StockingRegion->GetSelectedTriangleCount() == 0 && !SourceMesh->GetRefSkeleton().IsEmpty())
		{
			// 다리 본 키워드가 다르거나 필터 결과가 비어 있으면 본 가중치 선택으로 보완
			if (!LegBones.IsEmpty())
			{
				StockingRegion->SelectTrianglesByBones(*SourceMesh, LegBones, 0.15f);
			}
			else
			{
				TArray<int32> FallbackBones;
				FallbackBones.Add(0);
				StockingRegion->SelectTrianglesByBones(*SourceMesh, FallbackBones, 0.1f);
			}
		}
	}

	StockingComponent->SetSourceSkeletalMeshComponent(MeshComponent);
	if (!bHasPreloadedRegion)
	{
		const float Scale = UStockingRegion::ComputeReferenceScale(*SourceMesh, 0);
		StockingComponent->SetStockingRegion(StockingRegion);
		StockingComponent->CalibrateParametersForScale(Scale);
		ApplyStockingTearingSettings();
	}

	FString BuildError;
	const bool bBuilt = StockingComponent->RebuildStockingMesh(BuildError);
	if (bBuilt)
	{
		BuiltSourceMesh = SourceMesh;
		if (StockingInteractionComponent)
		{
			StockingInteractionComponent->SetTargetStockingComponent(StockingComponent);
		}
	}
	else
	{
		KRS_LOG(Warning, "AStockingActor: failed to build stocking mesh ({})", BuildError);
	}
	return bBuilt;
}

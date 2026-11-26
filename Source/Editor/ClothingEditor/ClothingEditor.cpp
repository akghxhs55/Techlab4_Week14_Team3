#include "EnginePCH.h"
#include "Editor/ClothingEditor/ClothingEditor.h"
#include "Editor/ClothingEditor/ClothingEditorTab.h"
#include "Asset/AssetManager.h"
#include "Asset/AssetUtils.h"
#include "Animation/AnimSequence.h"
#include "Collision/Ray.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StockingComponent.h"
#include "Components/StockingInteractionComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Physics/PhysScene.h"
#include "Physics/PhysicsSettings.h"
#include "Render/LineBatcher.h"
#include "Render/Material.h"
#include "Render/MaterialInstance.h"
#include "Render/Mesh.h"
#include "Render/SkeletalMesh.h"
#include "Render/SkeletalMeshObject.h"
#include "Render/StockingRegion.h"
#include "UObject/Property.h"
#include "UObject/UObjectGlobals.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <filesystem>
#include <functional>

namespace
{
	FString ToLowerAscii(const FString& Text)
	{
		FString Lower = Text;
		for (size_t i = 0; i < Lower.size(); ++i)
		{
			Lower[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(Lower[i])));
		}
		return Lower;
	}

	bool ContainsKeywordIgnoreCase(const FString& Text, const char* Keyword)
	{
		const FString LowerText = ToLowerAscii(Text);
		const FString LowerKey = ToLowerAscii(FString(Keyword));
		return LowerText.find(LowerKey) != FString::npos;
	}
}

FClothingEditor::FClothingEditor(
	UEditorEngine& InEditorEngine,
	FEditorManager& InEditorManager,
	const FString& InDocumentKey,
	const FString& InMeshAssetPath,
	USkeletalMesh& InMesh,
	const FString& InInitialRegionFile)
	: FSkeletalPreviewEditor(InEditorEngine, InEditorManager, InDocumentKey)
	, MeshAssetPath(InMeshAssetPath)
	, InitialRegionFile(InInitialRegionFile)
{
	Mesh = &InMesh;
}

TUniquePtr<IEditorTab> FClothingEditor::CreateTab()
{
	return MakeUnique<FClothingEditorTab>(*this);
}

bool FClothingEditor::InitializeResources()
{
	if (!InitializePreview())
	{
		return false;
	}

	SetBoneDrawMode(EBoneDrawMode::SelectedOnly);

	BoneGizmoTarget = Cast<USceneComponent>(PreviewScene.AddComponent(USceneComponent::StaticClass()));
	BoneGizmo.SetMode(EGizmoMode::Rotation);

	InteractionComponent = Cast<UStockingInteractionComponent>(
		PreviewScene.AddComponent(UStockingInteractionComponent::StaticClass()));
	if (Mesh)
	{
		CharacterScale = UStockingRegion::ComputeReferenceScale(*Mesh, 0);
		RegionBrushRadius = 0.06f * CharacterScale;
	}

	if (InteractionComponent)
	{
		InteractionComponent->bUseGameViewportInput = false;
		InteractionComponent->bSearchAllWorldStockings = true;
	}

	// 저장된 영역 파일이 있으면 우선 로드하고 없으면 다리 본 자동 선택 스타킹 생성
	FString RegionToLoad = InitialRegionFile;
	if (RegionToLoad.empty())
	{
		const FString DefaultFile = ResolveDefaultRegionFilePath();
		if (!DefaultFile.empty() && std::filesystem::exists(DefaultFile))
		{
			RegionToLoad = DefaultFile;
		}
	}

	if (!RegionToLoad.empty())
	{
		AddStockingLayer("Stocking", false, RegionToLoad);
	}
	else
	{
		AddStockingLayer("Stocking", true);
	}

	return true;
}

void FClothingEditor::ShutdownResources()
{
	BoneGizmo.EndDrag();
	BoneGizmo.SetTarget(nullptr);
	BoneGizmoTarget = nullptr;

	if (InteractionComponent)
	{
		InteractionComponent->EndGrab();
		InteractionComponent = nullptr;
	}

	UWorld* World = PreviewScene.GetWorld();
	for (int32 i = 0; i < Layers.Num(); ++i)
	{
		if (Layers[i]->StockingComponent && World)
		{
			if (AActor* Owner = Layers[i]->StockingComponent->GetOwner())
			{
				World->DestroyActor(Owner);
			}
		}
		if (Layers[i]->ClothComponent && World)
		{
			if (AActor* Owner = Layers[i]->ClothComponent->GetOwner())
			{
				World->DestroyActor(Owner);
			}
		}
	}
	Layers.Empty();
	SelectedLayerIndex = -1;
	CurrentAnimation = nullptr;
	CurrentAnimationPath.clear();

	FSkeletalPreviewEditor::ShutdownResources();
}

void FClothingEditor::TickBeforeWorld(float DeltaTime)
{
	(void)DeltaTime;
}

void FClothingEditor::TickAfterWorld(float DeltaTime)
{
	if (ViewportTool == EClothingViewportTool::PullTest)
	{
		BoneGizmo.EndDrag();
		BoneGizmo.SetTarget(nullptr);
		UpdatePullInteraction();
	}
	else if (ViewportTool == EClothingViewportTool::TearBrush)
	{
		BoneGizmo.EndDrag();
		BoneGizmo.SetTarget(nullptr);
		UpdateTearBrushInput();
	}
	else if (ViewportTool == EClothingViewportTool::PaintRegion)
	{
		BoneGizmo.EndDrag();
		BoneGizmo.SetTarget(nullptr);
		UpdateRegionBrushInput();
	}
	else
	{
		const bool bGizmoConsumed = UpdateBoneGizmo();
		if (!bGizmoConsumed)
		{
			UpdateBonePicking();
		}
		SyncBoneGizmo();
	}

	// 원본 애니메이션과 본 갱신 완료 직후 모든 스타킹 스키닝 동기화
	SyncAllStockingsToCharacter();

	// 프리뷰 물리 씬의 천 시뮬레이션 진행
	if (bSimulateClothPhysics)
	{
		StepPreviewClothPhysics(DeltaTime);
	}
}

const FGizmo* FClothingEditor::GetPreviewGizmo() const
{
	if (ViewportTool != EClothingViewportTool::PickBone || !BoneGizmo.GetTarget())
	{
		return nullptr;
	}
	return &BoneGizmo;
}

void FClothingEditor::SetGizmoMode(EGizmoMode Mode)
{
	BoneGizmo.EndDrag();
	BoneGizmo.SetMode(Mode);
}

bool FClothingEditor::IsViewportInteractionBusy() const
{
	if (BoneGizmo.IsUsing())
	{
		return true;
	}
	if (InteractionComponent && InteractionComponent->IsGrabbing())
	{
		return true;
	}
	if ((ViewportTool == EClothingViewportTool::PaintRegion || ViewportTool == EClothingViewportTool::TearBrush) &&
		PointerInput.bMouseDown && !CameraInput.bCaptured)
	{
		return true;
	}
	return false;
}

void FClothingEditor::OnBoneSelected()
{
	BoneGizmo.EndDrag();
	SyncBoneGizmo();
}

void FClothingEditor::CancelViewportInteraction()
{
	BoneGizmo.EndDrag();
	if (InteractionComponent)
	{
		InteractionComponent->EndGrab();
	}
}

FString FClothingEditor::ResolveDefaultRegionFilePath() const
{
	const FString MeshFile = FAssetUtils::ResolveAssetFilePath(MeshAssetPath);
	return MeshFile.empty() ? FString() : (MeshFile + ".stockingregion");
}

FString FClothingEditor::FindFirstMatchingBone(const TArray<const char*>& Keywords) const
{
	if (!Mesh)
	{
		return FString();
	}

	const TArray<FBoneInfo>& Bones = Mesh->GetRefSkeleton();
	for (int32 k = 0; k < Keywords.Num(); ++k)
	{
		for (int32 i = 0; i < Bones.Num(); ++i)
		{
			if (ContainsKeywordIgnoreCase(Bones[i].Name, Keywords[k]))
			{
				return Bones[i].Name;
			}
		}
	}

	return Bones.IsEmpty() ? FString() : Bones[0].Name;
}

FClothingLayer* FClothingEditor::GetLayer(int32 LayerIndex)
{
	if (LayerIndex < 0 || LayerIndex >= Layers.Num())
	{
		return nullptr;
	}
	return Layers[LayerIndex].get();
}

const FClothingLayer* FClothingEditor::GetLayer(int32 LayerIndex) const
{
	if (LayerIndex < 0 || LayerIndex >= Layers.Num())
	{
		return nullptr;
	}
	return Layers[LayerIndex].get();
}

int32 FClothingEditor::AddStockingLayer(const FString& InName, bool bAutoSelectLegs, const FString& RegionFilePath)
{
	if (!Mesh || !MeshComponent)
	{
		return -1;
	}

	TUniquePtr<FClothingLayer> NewLayer = MakeUnique<FClothingLayer>();
	NewLayer->Name = InName;
	NewLayer->Kind = EClothingLayerKind::Stocking;
	NewLayer->StockingRegion = NewObject<UStockingRegion>();
	NewLayer->StockingRegion->BindToSourceMesh(*Mesh, MeshAssetPath, 0);
	NewLayer->StockingRegion->SubdivisionIterations = 1;
	NewLayer->StockingMaterialInstance = FStockingMaterialLibrary::CreateStockingMaterialInstance(NewLayer->StockingMatParams);

	UStockingComponent* StockingComp = Cast<UStockingComponent>(
		PreviewScene.AddComponent(UStockingComponent::StaticClass()));
	if (!StockingComp)
	{
		return -1;
	}

	StockingComp->SetSourceSkeletalMeshComponent(MeshComponent);
	StockingComp->SetStockingRegion(NewLayer->StockingRegion);
	StockingComp->CalibrateParametersForScale(CharacterScale);
	StockingComp->SetStockingMaterial(NewLayer->StockingMaterialInstance);
	StockingComp->SetVisibility(true);
	NewLayer->StockingComponent = StockingComp;

	const int32 Index = Layers.Num();
	Layers.Add(std::move(NewLayer));
	SelectLayer(Index);

	FString Status;
	if (!RegionFilePath.empty())
	{
		LoadStockingLayerRegion(Index, RegionFilePath, Status);
	}
	else if (bAutoSelectLegs)
	{
		AutoSelectLegTriangles(Index);
	}
	else
	{
		Layers[Index]->LastStatusMessage = "Select triangles or choose a bone to build the stocking.";
	}

	return Index;
}

int32 FClothingEditor::AddCapeClothLayer(const FString& InName)
{
	if (!Mesh || !MeshComponent)
	{
		return -1;
	}

	TUniquePtr<FClothingLayer> NewLayer = MakeUnique<FClothingLayer>();
	NewLayer->Name = InName;
	NewLayer->Kind = EClothingLayerKind::ClothCape;
	NewLayer->ClothColor = FVector4(0.58f, 0.09f, 0.11f, 1.0f);
	NewLayer->ClothLocalOffset = FVector(-0.22f, 0.0f, 1.38f);

	TArray<const char*> PinCandidates;
	PinCandidates.Add("spine_03");
	PinCandidates.Add("spine_02");
	PinCandidates.Add("chest");
	PinCandidates.Add("spine");
	NewLayer->PinBoneName = FindFirstMatchingBone(PinCandidates);

	UClothComponent* ClothComp = Cast<UClothComponent>(
		PreviewScene.AddComponent(UClothComponent::StaticClass()));
	if (!ClothComp)
	{
		return -1;
	}

	ClothComp->Preset = EClothPreset::Grid;
	ClothComp->ClothWidth = 0.38f;
	ClothComp->BottomWidth = 0.86f;
	ClothComp->ClothHeight = 1.14f;
	ClothComp->SegmentsX = 12;
	ClothComp->SegmentsY = 16;
	ClothComp->Damping = FVector(0.04f, 0.04f, 0.04f);
	ClothComp->StretchStiffness = 1.0f;
	ClothComp->BendStiffness = 0.18f;
	ClothComp->ShearStiffness = 0.55f;
	ClothComp->DragCoefficient = 0.25f;
	ClothComp->LiftCoefficient = 0.10f;
	ClothComp->LinearInertiaScale = 0.55f;
	ClothComp->AngularInertiaScale = 0.30f;
	ClothComp->CentrifugalInertiaScale = 0.15f;
	ClothComp->bUseVirtualParticles = true;
	ClothComp->bEnableContinuousCollision = true;
	ClothComp->SetRelativeLocation(NewLayer->ClothLocalOffset);
	NewLayer->ClothComponent = ClothComp;

	if (UMaterial* DefaultMat = UAssetManager::GetAssetByPath<UMaterial>("DefaultMaterial"))
	{
		NewLayer->ClothMaterialInstance = UMaterial::CreateDynamicInstance(DefaultMat);
	}

	PopulateDefaultBodyCapsules(*NewLayer, true);

	const int32 Index = Layers.Num();
	Layers.Add(std::move(NewLayer));
	SelectLayer(Index);
	RebuildClothLayer(Index);
	ApplyClothLayerColor(Index);
	return Index;
}

int32 FClothingEditor::AddSkirtClothLayer(const FString& InName)
{
	if (!Mesh || !MeshComponent)
	{
		return -1;
	}

	TUniquePtr<FClothingLayer> NewLayer = MakeUnique<FClothingLayer>();
	NewLayer->Name = InName;
	NewLayer->Kind = EClothingLayerKind::ClothSkirt;
	NewLayer->ClothColor = FVector4(0.14f, 0.18f, 0.38f, 1.0f);
	NewLayer->ClothLocalOffset = FVector(0.02f, 0.0f, 0.96f);

	TArray<const char*> PinCandidates;
	PinCandidates.Add("pelvis");
	PinCandidates.Add("hips");
	PinCandidates.Add("waist");
	NewLayer->PinBoneName = FindFirstMatchingBone(PinCandidates);

	UClothComponent* ClothComp = Cast<UClothComponent>(
		PreviewScene.AddComponent(UClothComponent::StaticClass()));
	if (!ClothComp)
	{
		return -1;
	}

	ClothComp->Preset = EClothPreset::Cone;
	ClothComp->TopRadius = 0.145f;
	ClothComp->BottomRadius = 0.46f;
	ClothComp->ClothHeight = 0.44f;
	ClothComp->SegmentsX = 24;
	ClothComp->SegmentsY = 10;
	ClothComp->Damping = FVector(0.05f, 0.05f, 0.05f);
	ClothComp->StretchStiffness = 1.0f;
	ClothComp->BendStiffness = 0.18f;
	ClothComp->ShearStiffness = 0.55f;
	ClothComp->DragCoefficient = 0.30f;
	ClothComp->LiftCoefficient = 0.12f;
	ClothComp->LinearInertiaScale = 0.55f;
	ClothComp->AngularInertiaScale = 0.35f;
	ClothComp->CentrifugalInertiaScale = 0.25f;
	ClothComp->bUseVirtualParticles = true;
	ClothComp->bEnableContinuousCollision = true;
	ClothComp->SetRelativeLocation(NewLayer->ClothLocalOffset);
	NewLayer->ClothComponent = ClothComp;

	if (UMaterial* DefaultMat = UAssetManager::GetAssetByPath<UMaterial>("DefaultMaterial"))
	{
		NewLayer->ClothMaterialInstance = UMaterial::CreateDynamicInstance(DefaultMat);
	}

	PopulateDefaultBodyCapsules(*NewLayer, false);

	const int32 Index = Layers.Num();
	Layers.Add(std::move(NewLayer));
	SelectLayer(Index);
	RebuildClothLayer(Index);
	ApplyClothLayerColor(Index);
	return Index;
}

int32 FClothingEditor::AddStaticMeshClothLayer(const FString& InName)
{
	if (!Mesh || !MeshComponent)
	{
		return -1;
	}

	TUniquePtr<FClothingLayer> NewLayer = MakeUnique<FClothingLayer>();
	NewLayer->Name = InName;
	NewLayer->Kind = EClothingLayerKind::ClothStaticMesh;
	NewLayer->ClothColor = FVector4(0.32f, 0.38f, 0.44f, 1.0f);
	NewLayer->ClothLocalOffset = FVector(0.0f, 0.0f, 1.2f);

	TArray<const char*> PinCandidates;
	PinCandidates.Add("spine_03");
	PinCandidates.Add("pelvis");
	NewLayer->PinBoneName = FindFirstMatchingBone(PinCandidates);

	UClothComponent* ClothComp = Cast<UClothComponent>(
		PreviewScene.AddComponent(UClothComponent::StaticClass()));
	if (!ClothComp)
	{
		return -1;
	}

	ClothComp->Preset = EClothPreset::StaticMesh;
	ClothComp->PinHeightRatio = 0.90f;
	ClothComp->bEnableContinuousCollision = true;
	ClothComp->bUseVirtualParticles = true;
	ClothComp->SetRelativeLocation(NewLayer->ClothLocalOffset);
	NewLayer->ClothComponent = ClothComp;

	if (UMaterial* DefaultMat = UAssetManager::GetAssetByPath<UMaterial>("DefaultMaterial"))
	{
		NewLayer->ClothMaterialInstance = UMaterial::CreateDynamicInstance(DefaultMat);
	}

	PopulateDefaultBodyCapsules(*NewLayer, true);

	const int32 Index = Layers.Num();
	Layers.Add(std::move(NewLayer));
	SelectLayer(Index);
	RebuildClothLayer(Index);
	ApplyClothLayerColor(Index);
	return Index;
}

void FClothingEditor::RemoveLayer(int32 LayerIndex)
{
	FClothingLayer* Layer = GetLayer(LayerIndex);
	if (!Layer)
	{
		return;
	}

	if (InteractionComponent && InteractionComponent->GetActiveStocking() == Layer->StockingComponent)
	{
		InteractionComponent->EndGrab();
	}

	UWorld* World = PreviewScene.GetWorld();
	if (Layer->StockingComponent && World)
	{
		if (AActor* Owner = Layer->StockingComponent->GetOwner())
		{
			World->DestroyActor(Owner);
		}
	}
	if (Layer->ClothComponent && World)
	{
		if (AActor* Owner = Layer->ClothComponent->GetOwner())
		{
			World->DestroyActor(Owner);
		}
	}

	Layers.RemoveAt(LayerIndex, 1);
	if (Layers.IsEmpty())
	{
		SelectedLayerIndex = -1;
	}
	else if (SelectedLayerIndex >= Layers.Num())
	{
		SelectedLayerIndex = Layers.Num() - 1;
	}
}

void FClothingEditor::SelectLayer(int32 LayerIndex)
{
	if (LayerIndex < -1 || LayerIndex >= Layers.Num())
	{
		return;
	}
	SelectedLayerIndex = LayerIndex;

	if (InteractionComponent)
	{
		FClothingLayer* Layer = GetSelectedLayer();
		if (Layer && Layer->Kind == EClothingLayerKind::Stocking)
		{
			InteractionComponent->SetTargetStockingComponent(Layer->StockingComponent);
		}
		else
		{
			InteractionComponent->SetTargetStockingComponent(nullptr);
		}
	}
}

void FClothingEditor::SetLayerVisible(int32 LayerIndex, bool bVisible)
{
	FClothingLayer* Layer = GetLayer(LayerIndex);
	if (!Layer)
	{
		return;
	}
	Layer->bVisible = bVisible;
	if (Layer->StockingComponent)
	{
		Layer->StockingComponent->SetVisibility(bVisible);
	}
	if (Layer->ClothComponent)
	{
		Layer->ClothComponent->SetVisibility(bVisible);
	}
}

bool FClothingEditor::RebuildStockingLayer(int32 LayerIndex, FString& OutMessage)
{
	OutMessage.clear();
	FClothingLayer* Layer = GetLayer(LayerIndex);
	if (!Layer || Layer->Kind != EClothingLayerKind::Stocking || !Layer->StockingComponent || !Layer->StockingRegion)
	{
		OutMessage = "Invalid stocking layer.";
		return false;
	}

	Layer->StockingComponent->SetStockingRegion(Layer->StockingRegion);
	Layer->StockingComponent->SetStockingMaterial(Layer->StockingMaterialInstance);

	FString Error;
	const bool bOk = Layer->StockingComponent->RebuildStockingMesh(Error);
	Layer->bRegionValid = bOk;
	if (bOk)
	{
		OutMessage = "Stocking mesh built (" + std::to_string(Layer->StockingRegion->GetSelectedTriangleCount()) + " source tris).";
	}
	else
	{
		OutMessage = Error;
	}
	Layer->LastStatusMessage = OutMessage;
	return bOk;
}

bool FClothingEditor::ValidateStockingLayer(int32 LayerIndex, FString& OutMessage)
{
	OutMessage.clear();
	FClothingLayer* Layer = GetLayer(LayerIndex);
	if (!Layer || Layer->Kind != EClothingLayerKind::Stocking || !Layer->StockingComponent)
	{
		OutMessage = "Invalid stocking layer.";
		return false;
	}

	const bool bValid = Layer->StockingComponent->ValidateAgainstSourceMesh(OutMessage);
	Layer->bRegionValid = bValid;
	if (bValid)
	{
		OutMessage = "Region and skeleton are valid.";
	}
	Layer->LastStatusMessage = OutMessage;
	return bValid;
}

bool FClothingEditor::SaveStockingLayerRegion(int32 LayerIndex, FString& OutMessage)
{
	OutMessage.clear();
	FClothingLayer* Layer = GetLayer(LayerIndex);
	if (!Layer || Layer->Kind != EClothingLayerKind::Stocking || !Layer->StockingRegion || !Mesh)
	{
		OutMessage = "Cannot save stocking region.";
		return false;
	}

	UStockingRegion* Region = Layer->StockingRegion;
	Region->BindToSourceMesh(*Mesh, MeshAssetPath, 0);
	Region->SavedMaterialParams = Layer->StockingMatParams;
	Region->bHasSavedMaterialParams = true;
	Region->SavedLegRadiusRatio = AutoLegMaxRadiusRatio;
	Region->SavedLegHeightRatio = AutoLegMaxHeightRatio;
	Region->bHasSavedLegFilter = true;

	if (Layer->StockingComponent)
	{
		Region->SurfaceOffset = Layer->StockingComponent->SurfaceOffset;
		Region->SavedPullRadius = Layer->StockingComponent->PullRadius;
		Region->SavedMaxPullDistance = Layer->StockingComponent->MaxPullDistance;
		Region->SavedPullFalloffExponent = Layer->StockingComponent->PullFalloffExponent;
		Region->SavedWrinkleAmplitude = Layer->StockingComponent->WrinkleAmplitude;
		Region->SavedWrinkleFrequency = Layer->StockingComponent->WrinkleFrequency;
		Region->SavedSpringStiffness = Layer->StockingComponent->SpringStiffness;
		Region->SavedSpringDamping = Layer->StockingComponent->SpringDamping;
		Region->SavedBoundsPadding = Layer->StockingComponent->BoundsPadding;
		Region->bHasSavedPullParams = true;

		Region->SavedUseNvClothSimulation = Layer->StockingComponent->bUseNvClothSimulation;
		Region->SavedClothJointWrinkleSlack = Layer->StockingComponent->ClothJointWrinkleSlack;
		Region->SavedClothBackstopRadius = Layer->StockingComponent->ClothBackstopRadius;
		Region->SavedClothStretchStiffness = Layer->StockingComponent->ClothStretchStiffness;
		Region->SavedClothBendStiffness = Layer->StockingComponent->ClothBendStiffness;
		Region->SavedClothShearStiffness = Layer->StockingComponent->ClothShearStiffness;
		Region->SavedClothCompressionLimit = Layer->StockingComponent->ClothCompressionLimit;
		Region->SavedClothStretchLimit = Layer->StockingComponent->ClothStretchLimit;
		Region->SavedClothMotionStiffness = Layer->StockingComponent->ClothMotionStiffness;
		Region->SavedClothDamping = Layer->StockingComponent->ClothDamping;
		Region->SavedClothSelfCollisionDistance = Layer->StockingComponent->ClothSelfCollisionDistance;
		Region->SavedClothSelfCollisionStiffness = Layer->StockingComponent->ClothSelfCollisionStiffness;
		Region->bHasSavedClothWrinkleParams = true;
	}

	const FString FilePath = Region->GetRegionFilePath().empty()
		? ResolveDefaultRegionFilePath()
		: Region->GetRegionFilePath();

	if (FilePath.empty() || !Region->SaveToFile(FilePath))
	{
		OutMessage = "Failed to save region file: " + FilePath;
		Layer->LastStatusMessage = OutMessage;
		return false;
	}

	Region->SetRegionFilePath(FilePath);
	OutMessage = "Saved (" + std::to_string(Region->GetSelectedTriangleCount()) + " tris): " +
		std::filesystem::path(FilePath).filename().string();
	Layer->LastStatusMessage = OutMessage;
	return true;
}

bool FClothingEditor::LoadStockingLayerRegion(int32 LayerIndex, const FString& FilePath, FString& OutMessage)
{
	OutMessage.clear();
	FClothingLayer* Layer = GetLayer(LayerIndex);
	if (!Layer || Layer->Kind != EClothingLayerKind::Stocking || !Layer->StockingRegion)
	{
		OutMessage = "Invalid stocking layer.";
		return false;
	}

	UStockingRegion* Region = Layer->StockingRegion;
	FString TargetFile = FilePath;
	if (TargetFile.empty())
	{
		TargetFile = Region->GetRegionFilePath().empty()
			? ResolveDefaultRegionFilePath()
			: Region->GetRegionFilePath();
	}

	if (!Region->LoadFromFile(TargetFile, OutMessage))
	{
		Layer->bRegionValid = false;
		Layer->LastStatusMessage = OutMessage;
		return false;
	}

	if (Region->bHasSavedMaterialParams)
	{
		Layer->StockingMatParams = Region->SavedMaterialParams;
		ApplyStockingMaterialParams(LayerIndex);
	}
	if (Region->bHasSavedLegFilter)
	{
		SetAutoLegMaxRadiusRatio(Region->SavedLegRadiusRatio);
		SetAutoLegMaxHeightRatio(Region->SavedLegHeightRatio);
	}
	if (Layer->StockingComponent)
	{
		Layer->StockingComponent->SurfaceOffset = Region->SurfaceOffset;
		if (Region->bHasSavedPullParams)
		{
			Layer->StockingComponent->PullRadius = Region->SavedPullRadius;
			Layer->StockingComponent->MaxPullDistance = Region->SavedMaxPullDistance;
			Layer->StockingComponent->PullFalloffExponent = Region->SavedPullFalloffExponent;
			Layer->StockingComponent->WrinkleAmplitude = Region->SavedWrinkleAmplitude;
			Layer->StockingComponent->WrinkleFrequency = Region->SavedWrinkleFrequency;
			Layer->StockingComponent->SpringStiffness = Region->SavedSpringStiffness;
			Layer->StockingComponent->SpringDamping = Region->SavedSpringDamping;
			Layer->StockingComponent->BoundsPadding = Region->SavedBoundsPadding;
		}
		if (Region->bHasSavedClothWrinkleParams)
		{
			Layer->StockingComponent->bUseNvClothSimulation = Region->SavedUseNvClothSimulation;
			Layer->StockingComponent->ClothJointWrinkleSlack = Region->SavedClothJointWrinkleSlack;
			Layer->StockingComponent->ClothBackstopRadius = Region->SavedClothBackstopRadius;
			Layer->StockingComponent->ClothStretchStiffness = Region->SavedClothStretchStiffness;
			Layer->StockingComponent->ClothBendStiffness = Region->SavedClothBendStiffness;
			Layer->StockingComponent->ClothShearStiffness = Region->SavedClothShearStiffness;
			Layer->StockingComponent->ClothCompressionLimit = Region->SavedClothCompressionLimit;
			Layer->StockingComponent->ClothStretchLimit = Region->SavedClothStretchLimit;
			Layer->StockingComponent->ClothMotionStiffness = Region->SavedClothMotionStiffness;
			Layer->StockingComponent->ClothDamping = Region->SavedClothDamping;
			Layer->StockingComponent->ClothSelfCollisionDistance = Region->SavedClothSelfCollisionDistance;
			Layer->StockingComponent->ClothSelfCollisionStiffness = Region->SavedClothSelfCollisionStiffness;
			Layer->StockingComponent->ApplyNvClothParams();
		}
	}

	const bool bBuilt = RebuildStockingLayer(LayerIndex, OutMessage);
	if (bBuilt)
	{
		OutMessage = "Loaded (" + std::to_string(Region->GetSelectedTriangleCount()) + " tris): " +
			std::filesystem::path(TargetFile).filename().string();
		Layer->LastStatusMessage = OutMessage;
	}
	return bBuilt;
}

void FClothingEditor::ApplyStockingMaterialParams(int32 LayerIndex)
{
	FClothingLayer* Layer = GetLayer(LayerIndex);
	if (!Layer || Layer->Kind != EClothingLayerKind::Stocking || !Layer->StockingMaterialInstance)
	{
		return;
	}
	FStockingMaterialLibrary::ApplyParameters(Layer->StockingMaterialInstance, Layer->StockingMatParams);
	if (Layer->StockingComponent)
	{
		Layer->StockingComponent->SyncTearMaskBinding();
		Layer->StockingComponent->MarkRenderStateDirty();
	}
}

void FClothingEditor::SelectStockingTrianglesByBone(int32 LayerIndex, int32 BoneIndex, float MinWeightThreshold)
{
	FClothingLayer* Layer = GetLayer(LayerIndex);
	if (!Layer || Layer->Kind != EClothingLayerKind::Stocking || !Layer->StockingRegion || !Mesh || BoneIndex < 0)
	{
		return;
	}
	TArray<int32> Bones;
	Bones.Add(BoneIndex);
	Layer->StockingRegion->SelectTrianglesByBones(*Mesh, Bones, MinWeightThreshold);
	FString Status;
	RebuildStockingLayer(LayerIndex, Status);
}

void FClothingEditor::AutoSelectLegTriangles(int32 LayerIndex)
{
	FClothingLayer* Layer = GetLayer(LayerIndex);
	if (!Layer || Layer->Kind != EClothingLayerKind::Stocking || !Layer->StockingRegion || !Mesh)
	{
		return;
	}

	const TArray<FBoneInfo>& Bones = Mesh->GetRefSkeleton();
	TArray<int32> LegBones;
	for (int32 i = 0; i < Bones.Num(); ++i)
	{
		if (ContainsKeywordIgnoreCase(Bones[i].Name, "thigh") ||
			ContainsKeywordIgnoreCase(Bones[i].Name, "calf") ||
			ContainsKeywordIgnoreCase(Bones[i].Name, "shin") ||
			ContainsKeywordIgnoreCase(Bones[i].Name, "knee") ||
			ContainsKeywordIgnoreCase(Bones[i].Name, "foot") ||
			ContainsKeywordIgnoreCase(Bones[i].Name, "leg"))
		{
			LegBones.Add(i);
		}
	}

	Layer->StockingRegion->ClearTriangles();
	if (!LegBones.IsEmpty())
	{
		Layer->StockingRegion->SelectTrianglesByBoneFilter(
			*Mesh,
			LegBones,
			0.25f,
			AutoLegMaxRadiusRatio,
			AutoLegMaxHeightRatio);
	}
	else if (!Bones.IsEmpty())
	{
		// 다리 본 이름이 다르면 기본 본 삼각형을 선택해 바로 확인 가능하도록 구성
		TArray<int32> FallbackBones;
		FallbackBones.Add(0);
		Layer->StockingRegion->SelectTrianglesByBones(*Mesh, FallbackBones, 0.1f);
	}

	FString Status;
	RebuildStockingLayer(LayerIndex, Status);
}

void FClothingEditor::GrowStockingLayerRegion(int32 LayerIndex)
{
	FClothingLayer* Layer = GetLayer(LayerIndex);
	if (!Layer || Layer->Kind != EClothingLayerKind::Stocking || !Layer->StockingRegion || !Mesh)
	{
		return;
	}
	Layer->StockingRegion->GrowSelection(*Mesh);
	FString Status;
	RebuildStockingLayer(LayerIndex, Status);
}

void FClothingEditor::ShrinkStockingLayerRegion(int32 LayerIndex)
{
	FClothingLayer* Layer = GetLayer(LayerIndex);
	if (!Layer || Layer->Kind != EClothingLayerKind::Stocking || !Layer->StockingRegion || !Mesh)
	{
		return;
	}
	Layer->StockingRegion->ShrinkSelection(*Mesh);
	FString Status;
	RebuildStockingLayer(LayerIndex, Status);
}

void FClothingEditor::ClearStockingLayerRegion(int32 LayerIndex)
{
	FClothingLayer* Layer = GetLayer(LayerIndex);
	if (!Layer || Layer->Kind != EClothingLayerKind::Stocking || !Layer->StockingRegion)
	{
		return;
	}
	Layer->StockingRegion->ClearTriangles();
	FString Status;
	RebuildStockingLayer(LayerIndex, Status);
}

void FClothingEditor::PopulateDefaultBodyCapsules(FClothingLayer& Layer, bool bUpperBody)
{
	Layer.BoneCapsules.Reset();
	if (!Mesh)
	{
		return;
	}

	const std::function<void(const char*, const char*, float, float)> TryAddCapsule =
		[&](const char* BoneA, const char* BoneB, float RadiusA, float RadiusB)
	{
		if (Mesh->FindBoneIndex(BoneA) >= 0 && Mesh->FindBoneIndex(BoneB) >= 0)
		{
			FClothBoneCapsule Cap;
			Cap.BoneA = BoneA;
			Cap.BoneB = BoneB;
			Cap.RadiusA = RadiusA;
			Cap.RadiusB = RadiusB;
			Layer.BoneCapsules.Add(Cap);
		}
	};

	TryAddCapsule("pelvis", "spine_01", 0.14f, 0.14f);
	if (bUpperBody)
	{
		TryAddCapsule("spine_01", "spine_03", 0.15f, 0.16f);
		TryAddCapsule("spine_03", "head", 0.12f, 0.11f);
		TryAddCapsule("clavicle_l", "upperarm_l", 0.11f, 0.10f);
		TryAddCapsule("upperarm_l", "lowerarm_l", 0.09f, 0.08f);
		TryAddCapsule("clavicle_r", "upperarm_r", 0.11f, 0.10f);
		TryAddCapsule("upperarm_r", "lowerarm_r", 0.09f, 0.08f);
	}
	else
	{
		TryAddCapsule("spine_01", "spine_02", 0.12f, 0.12f);
	}
	TryAddCapsule("thigh_l", "calf_l", 0.11f, 0.09f);
	TryAddCapsule("calf_l", "foot_l", 0.09f, 0.07f);
	TryAddCapsule("thigh_r", "calf_r", 0.11f, 0.09f);
	TryAddCapsule("calf_r", "foot_r", 0.09f, 0.07f);
}

void FClothingEditor::RebuildClothLayer(int32 LayerIndex)
{
	FClothingLayer* Layer = GetLayer(LayerIndex);
	if (!Layer || !Layer->ClothComponent || !MeshComponent)
	{
		return;
	}

	UWorld* World = PreviewScene.GetWorld();
	UClothComponent* ClothComp = Layer->ClothComponent;
	const bool bWasRegistered = ClothComp->IsRegistered();
	if (bWasRegistered)
	{
		ClothComp->UnregisterComponent();
	}

	if (Layer->Kind == EClothingLayerKind::ClothStaticMesh && !Layer->StaticMeshAssetPath.empty())
	{
		ClothComp->SourceStaticMesh = UAssetManager::GetAssetByPath<UStaticMesh>(Layer->StaticMeshAssetPath);
	}

	ClothComp->SetRelativeLocation(Layer->ClothLocalOffset);
	ClothComp->SetTargetSkeletalMesh(MeshComponent);
	ClothComp->RebuildClothMesh();

	ClothComp->ClearBonePins();
	if (!Layer->PinBoneName.empty() && Layer->Kind != EClothingLayerKind::ClothStaticMesh)
	{
		ClothComp->PinRowToBone(0, Layer->PinBoneName);
	}

	ClothComp->ClearBoneCapsuleColliders();
	for (int32 i = 0; i < Layer->BoneCapsules.Num(); ++i)
	{
		const FClothBoneCapsule& Cap = Layer->BoneCapsules[i];
		ClothComp->AddBoneCapsuleCollider(Cap.BoneA, Cap.BoneB, Cap.RadiusA, Cap.RadiusB);
	}

	if (bWasRegistered && World)
	{
		ClothComp->RegisterComponentWithWorld(World);
	}
	ApplyClothLayerColor(LayerIndex);
	ClothComp->SetVisibility(Layer->bVisible);
}

void FClothingEditor::ApplyClothLayerParams(int32 LayerIndex, const char* ChangedPropertyName)
{
	FClothingLayer* Layer = GetLayer(LayerIndex);
	if (!Layer || !Layer->ClothComponent)
	{
		return;
	}

	// 물리 파라미터 갱신을 컴포넌트에 통지
	FFloatProperty NotifyProp;
	NotifyProp.Name = ChangedPropertyName ? ChangedPropertyName : "StretchStiffness";
	Layer->ClothComponent->PostEditChangeProperty(NotifyProp);
}

void FClothingEditor::ApplyClothLayerColor(int32 LayerIndex)
{
	FClothingLayer* Layer = GetLayer(LayerIndex);
	if (!Layer || !Layer->ClothComponent || !Layer->ClothMaterialInstance)
	{
		return;
	}
	Layer->ClothMaterialInstance->SetBaseColorOverride(Layer->ClothColor);
	Layer->ClothComponent->SetMaterial(0, Layer->ClothMaterialInstance);
	Layer->ClothComponent->MarkRenderStateDirty();
}

void FClothingEditor::ResetAllClothPoses()
{
	for (int32 i = 0; i < Layers.Num(); ++i)
	{
		if (Layers[i]->ClothComponent)
		{
			Layers[i]->ClothComponent->ResetClothPose();
		}
		if (Layers[i]->StockingComponent)
		{
			Layers[i]->StockingComponent->ResetPullImmediately();
			Layers[i]->StockingComponent->ResetClothSimulation();
		}
	}
}

void FClothingEditor::SetViewportTool(EClothingViewportTool InTool)
{
	if (ViewportTool == InTool)
	{
		return;
	}
	BoneGizmo.EndDrag();
	if (InteractionComponent)
	{
		InteractionComponent->EndGrab();
	}
	ViewportTool = InTool;
	if (ViewportTool == EClothingViewportTool::PickBone)
	{
		if (GetBoneDrawMode() == EBoneDrawMode::None)
		{
			SetBoneDrawMode(EBoneDrawMode::AllHierarchy);
		}
		SyncBoneGizmo();
	}
	else
	{
		BoneGizmo.SetTarget(nullptr);
	}
}

bool FClothingEditor::PlayAnimationByPath(const FString& AnimPath)
{
	if (!MeshComponent || AnimPath.empty())
	{
		return false;
	}

	UAnimSequence* Seq = UAssetManager::LoadAnimSequence(AnimPath);
	if (!Seq)
	{
		return false;
	}

	CurrentAnimation = Seq;
	CurrentAnimationPath = AnimPath;
	MeshComponent->PlayAnimation(Seq, bAnimLooping);
	return true;
}

void FClothingEditor::ToggleAnimationPlayback()
{
	if (!MeshComponent || !CurrentAnimation)
	{
		return;
	}
	if (MeshComponent->IsPlaying())
	{
		MeshComponent->Stop();
	}
	else if (!MeshComponent->GetAnimInstance())
	{
		MeshComponent->PlayAnimation(CurrentAnimation, bAnimLooping);
	}
	else
	{
		MeshComponent->Play(bAnimLooping);
	}
}

void FClothingEditor::StopAnimation()
{
	if (!MeshComponent)
	{
		return;
	}
	MeshComponent->Stop();
	CurrentAnimation = nullptr;
	CurrentAnimationPath.clear();
	MeshComponent->SetAnimInstance(nullptr);
	MeshComponent->ResetPose();
	MeshComponent->RefreshBoneTransformsNow();
	SyncAllStockingsToCharacter();
	SyncBoneGizmo();
}

bool FClothingEditor::IsAnimationPlaying() const
{
	return MeshComponent && MeshComponent->IsPlaying();
}

void FClothingEditor::SetAnimationLooping(bool bLoop)
{
	bAnimLooping = bLoop;
	if (MeshComponent && MeshComponent->IsPlaying())
	{
		MeshComponent->Play(bAnimLooping);
	}
}

bool FClothingEditor::GetSelectedBoneLocalTransform(FTransform& OutTransform) const
{
	if (!MeshComponent || SelectedBoneIndex < 0 || SelectedBoneIndex >= BoneItems.Num())
	{
		return false;
	}
	OutTransform = MeshComponent->GetBoneLocalTransform(SelectedBoneIndex);
	return true;
}

void FClothingEditor::SetSelectedBoneLocalTransform(const FTransform& Transform)
{
	if (!MeshComponent || SelectedBoneIndex < 0 || SelectedBoneIndex >= BoneItems.Num())
	{
		return;
	}
	BoneGizmo.EndDrag();
	MeshComponent->Stop();
	MeshComponent->SetAnimInstance(nullptr);
	MeshComponent->SetBoneLocalTransform(SelectedBoneIndex, Transform);
	MeshComponent->RefreshBoneTransformsNow();
	SyncAllStockingsToCharacter();
	SyncBoneGizmo();
}

void FClothingEditor::ResetCharacterPose()
{
	if (!MeshComponent)
	{
		return;
	}
	BoneGizmo.EndDrag();
	MeshComponent->Stop();
	MeshComponent->SetAnimInstance(nullptr);
	MeshComponent->ResetPose();
	MeshComponent->RefreshBoneTransformsNow();
	SyncAllStockingsToCharacter();
	SyncBoneGizmo();
	ResetAllClothPoses();
}

FQuat FClothingEditor::GetBoneWorldRotation(int32 BoneIndex) const
{
	FQuat Rotation = FQuat::Identity();
	if (!Mesh || !MeshComponent)
	{
		return Rotation;
	}
	const TArray<FBoneInfo>& Bones = Mesh->GetRefSkeleton();
	while (BoneIndex >= 0 && BoneIndex < Bones.Num())
	{
		Rotation = MeshComponent->GetBoneLocalTransform(BoneIndex).Rotation * Rotation;
		BoneIndex = Bones[BoneIndex].ParentIndex;
	}
	return (MeshComponent->GetWorldRotation().Quaternion() * Rotation).Normalized();
}

void FClothingEditor::SyncBoneGizmo()
{
	if (ViewportTool != EClothingViewportTool::PickBone || !MeshComponent || !BoneGizmoTarget ||
		SelectedBoneIndex < 0 || SelectedBoneIndex >= BoneItems.Num() || BoneDrawMode == EBoneDrawMode::None)
	{
		BoneGizmo.SetTarget(nullptr);
		return;
	}

	if (BoneGizmo.IsUsing())
	{
		return;
	}

	const FMatrix BoneWorld = MeshComponent->GetBoneWorldMatrix(SelectedBoneIndex);
	const FVector Location = BoneWorld.TransformPosition(FVector::ZeroVector);
	const FRotator Rotation = GetBoneWorldRotation(SelectedBoneIndex).ToFRotator();
	BoneGizmoTarget->SetTransform(FTransform(Rotation, Location));
	BoneGizmo.SetTarget(BoneGizmoTarget);
}

bool FClothingEditor::UpdateBoneGizmo()
{
	if (!CanProcessViewportInput() || !BoneGizmo.GetTarget() || !Mesh || !MeshComponent ||
		SelectedBoneIndex < 0 || SelectedBoneIndex >= BoneItems.Num())
	{
		BoneGizmo.EndDrag();
		return false;
	}

	if (PointerInput.bCancel)
	{
		if (BoneGizmo.IsUsing())
		{
			MeshComponent->SetBoneLocalTransform(SelectedBoneIndex, DragStartBoneTransform);
			MeshComponent->RefreshBoneTransformsNow();
			SyncAllStockingsToCharacter();
		}
		BoneGizmo.EndDrag();
		return false;
	}

	const bool bWasUsing = BoneGizmo.IsUsing();
	if ((!PointerInput.bHovered && !bWasUsing) || CameraInput.bCaptured)
	{
		BoneGizmo.EndDrag();
		return false;
	}

	SyncBoneGizmo();
	const int32 Width = static_cast<int32>(ViewportClient->GetWidth());
	const int32 Height = static_cast<int32>(ViewportClient->GetHeight());
	const FVector2 MousePosition(PointerInput.MouseUV.X * Width, PointerInput.MouseUV.Y * Height);
	const FRay MouseRay = ViewportClient->DeprojectScreenToWorld(MousePosition);

	const bool bUpdateDrag = bWasUsing || (PointerInput.bMousePressed && PointerInput.bMouseDown);
	BoneGizmo.Update(MouseRay, MousePosition, ViewportClient->GetViewProjectionMatrix(),
		Width, Height, bUpdateDrag, ViewportClient->GetViewLocation(), !ViewportClient->IsPerspective());

	if (!bWasUsing && BoneGizmo.IsUsing())
	{
		const int32 ParentIndex = Mesh->GetRefSkeleton()[SelectedBoneIndex].ParentIndex;
		const FMatrix ParentWorld = ParentIndex >= 0
			? MeshComponent->GetBoneWorldMatrix(ParentIndex)
			: MeshComponent->GetWorldMatrix();

		if (BoneGizmo.GetMode() == EGizmoMode::Location && std::fabs(ParentWorld.Determinant()) < 1e-12f)
		{
			BoneGizmo.EndDrag();
			return true;
		}

		DragStartBoneTransform = MeshComponent->GetBoneLocalTransform(SelectedBoneIndex);
		DragParentRotation = GetBoneWorldRotation(ParentIndex);
		if (BoneGizmo.GetMode() == EGizmoMode::Location)
		{
			DragParentInverse = ParentWorld.Inverse();
		}
	}

	if (BoneGizmo.IsUsing())
	{
		ApplyBoneGizmoTransform();
		if (!PointerInput.bMouseDown)
		{
			BoneGizmo.EndDrag();
		}
	}

	return bWasUsing || BoneGizmo.IsUsing() || BoneGizmo.GetHoveredAxis() >= 0;
}

void FClothingEditor::ApplyBoneGizmoTransform()
{
	if (!MeshComponent || !BoneGizmoTarget || SelectedBoneIndex < 0 || SelectedBoneIndex >= BoneItems.Num())
	{
		return;
	}

	FTransform Local = DragStartBoneTransform;
	switch (BoneGizmo.GetMode())
	{
	case EGizmoMode::Location:
		Local.Location = DragParentInverse.TransformPosition(BoneGizmoTarget->GetWorldLocation());
		break;
	case EGizmoMode::Rotation:
		Local.Rotation = (DragParentRotation.Inverse() *
			BoneGizmoTarget->GetRelativeRotation().Quaternion()).Normalized();
		break;
	case EGizmoMode::Scale:
		Local.Scale = DragStartBoneTransform.Scale * BoneGizmoTarget->GetRelativeScale3D();
		break;
	default:
		break;
	}

	MeshComponent->Stop();
	MeshComponent->SetAnimInstance(nullptr);
	MeshComponent->SetBoneLocalTransform(SelectedBoneIndex, Local);
	MeshComponent->RefreshBoneTransformsNow();
}

void FClothingEditor::UpdatePullInteraction()
{
	if (!InteractionComponent || !CanProcessViewportInput() || CameraInput.bCaptured ||
		ViewportWidth == 0 || ViewportHeight == 0)
	{
		if (InteractionComponent && InteractionComponent->IsGrabbing() && !PointerInput.bMouseDown)
		{
			InteractionComponent->EndGrab();
		}
		return;
	}

	if (PointerInput.bCancel)
	{
		InteractionComponent->EndGrab();
		return;
	}

	const FVector2 MousePosition(PointerInput.MouseUV.X * ViewportWidth, PointerInput.MouseUV.Y * ViewportHeight);
	const FRay WorldRay = ViewportClient->DeprojectScreenToWorld(MousePosition);

	if (PointerInput.bMousePressed && PointerInput.bHovered)
	{
		InteractionComponent->BeginGrabFromRay(WorldRay);
	}
	else if (InteractionComponent->IsGrabbing() && PointerInput.bMouseDown)
	{
		InteractionComponent->UpdateGrabFromRay(WorldRay);
	}
	else if (InteractionComponent->IsGrabbing())
	{
		InteractionComponent->EndGrab();
	}
}

void FClothingEditor::RepairAllStockingTears()
{
	for (int32 i = 0; i < Layers.Num(); ++i)
	{
		if (Layers[i]->Kind == EClothingLayerKind::Stocking && Layers[i]->StockingComponent)
		{
			Layers[i]->StockingComponent->RepairAllTears();
		}
	}
}

void FClothingEditor::UpdateTearBrushInput()
{
	if (!InteractionComponent || !CanProcessViewportInput() || !PointerInput.bHovered ||
		!PointerInput.bMouseDown || CameraInput.bCaptured || ViewportWidth == 0 || ViewportHeight == 0)
	{
		bHasLastTearHit = false;
		return;
	}

	const FVector2 MousePosition(PointerInput.MouseUV.X * ViewportWidth, PointerInput.MouseUV.Y * ViewportHeight);
	const FRay WorldRay = ViewportClient->DeprojectScreenToWorld(MousePosition);

	FStockingHitInfo Hit;
	if (!InteractionComponent->RaycastStocking(WorldRay, Hit) || !Hit.HitComponent)
	{
		bHasLastTearHit = false;
		return;
	}

	FVector DragDirLocal(0.0f, 0.0f, 1.0f);
	float Intensity = 0.85f;
	if (bHasLastTearHit)
	{
		const FVector Delta = Hit.LocalLocation - LastTearHitLocal;
		const float DeltaLen = Delta.Size();
		if (DeltaLen > 1e-4f * std::max(CharacterScale, 0.05f))
		{
			DragDirLocal = Delta / DeltaLen;
			Intensity = 0.55f;
		}
		else
		{
			Intensity = 0.28f;
		}
	}

	LastTearHitLocal = Hit.LocalLocation;
	bHasLastTearHit = true;
	Hit.HitComponent->ApplyTearAtSurfaceHit(Hit.TriangleIndex, Hit.Barycentric, DragDirLocal, Intensity);
}

void FClothingEditor::UpdateRegionBrushInput()
{
	FClothingLayer* Layer = GetSelectedLayer();
	if (!Layer || Layer->Kind != EClothingLayerKind::Stocking || !Layer->StockingRegion || !Mesh || !MeshComponent)
	{
		return;
	}

	if (!CanProcessViewportInput() || !PointerInput.bHovered || !PointerInput.bMouseDown ||
		CameraInput.bCaptured || ViewportWidth == 0 || ViewportHeight == 0)
	{
		return;
	}

	FSkeletalMeshObject* MeshObject = MeshComponent->GetMeshObject();
	const TArray<FVertex>* Vertices = MeshObject ? MeshObject->GetCPUSkinnedVertices() : nullptr;
	if (!Vertices || Mesh->GetRenderData().LODRenderData.IsEmpty())
	{
		return;
	}

	const FVector2 MousePosition(PointerInput.MouseUV.X * ViewportWidth, PointerInput.MouseUV.Y * ViewportHeight);
	const FRay WorldRay = ViewportClient->DeprojectScreenToWorld(MousePosition);
	const FRay LocalRay = ToLocalRay(WorldRay, MeshComponent->GetWorldMatrix());
	const TArray<uint32>& Indices = Mesh->GetRenderData().LODRenderData[0].Indices;
	const int32 TriCount = Indices.Num() / 3;

	float BestT = FLT_MAX;
	int32 HitTri = -1;
	for (int32 t = 0; t < TriCount; ++t)
	{
		float HitT = FLT_MAX;
		const FVector& P0 = (*Vertices)[Indices[t * 3]].Position;
		const FVector& P1 = (*Vertices)[Indices[t * 3 + 1]].Position;
		const FVector& P2 = (*Vertices)[Indices[t * 3 + 2]].Position;
		if (RayIntersectsTriangle(LocalRay, P0, P1, P2, HitT) && HitT < BestT)
		{
			BestT = HitT;
			HitTri = t;
		}
	}

	if (HitTri < 0)
	{
		return;
	}

	bool bModified = false;
	const FVector HitLocal = LocalRay.PointAt(BestT);
	const float RadiusSq = RegionBrushRadius * RegionBrushRadius;
	TArray<uint8> SelectedMask;
	SelectedMask.SetNum(TriCount);
	for (int32 i = 0; i < TriCount; ++i)
	{
		SelectedMask[i] = 0;
	}
	const TArray<uint32>& ExistingTris = Layer->StockingRegion->GetTriangleIndices();
	for (int32 i = 0; i < ExistingTris.Num(); ++i)
	{
		if (static_cast<int32>(ExistingTris[i]) < TriCount)
		{
			SelectedMask[static_cast<int32>(ExistingTris[i])] = 1;
		}
	}

	for (int32 t = 0; t < TriCount; ++t)
	{
		bool bMatch = (t == HitTri);
		if (!bMatch && RadiusSq > 1e-8f)
		{
			const FVector Center = (
				(*Vertices)[Indices[t * 3]].Position +
				(*Vertices)[Indices[t * 3 + 1]].Position +
				(*Vertices)[Indices[t * 3 + 2]].Position) * (1.0f / 3.0f);
			bMatch = (Center - HitLocal).SizeSquared() <= RadiusSq;
		}
		if (bMatch)
		{
			if (bRegionEraseMode)
			{
				if (SelectedMask[t] != 0)
				{
					SelectedMask[t] = 0;
					bModified = true;
				}
			}
			else if (SelectedMask[t] == 0)
			{
				SelectedMask[t] = 1;
				bModified = true;
			}
		}
	}

	if (bModified)
	{
		TArray<uint32> UpdatedTris;
		UpdatedTris.Reserve(ExistingTris.Num());
		for (int32 t = 0; t < TriCount; ++t)
		{
			if (SelectedMask[t] != 0)
			{
				UpdatedTris.Add(static_cast<uint32>(t));
			}
		}
		Layer->StockingRegion->SetTriangles(UpdatedTris);
		FString Status;
		RebuildStockingLayer(SelectedLayerIndex, Status);
	}
}

void FClothingEditor::SyncAllStockingsToCharacter()
{
	if (!MeshComponent)
	{
		return;
	}

	for (int32 i = 0; i < Layers.Num(); ++i)
	{
		if (Layers[i]->Kind == EClothingLayerKind::Stocking && Layers[i]->StockingComponent)
		{
			Layers[i]->StockingComponent->UpdateBoneTransformsNow();
		}
	}
}

void FClothingEditor::StepPreviewClothPhysics(float DeltaTime)
{
	UWorld* World = PreviewScene.GetWorld();
	FPhysScene* PhysScene = World ? World->GetPhysicsScene() : nullptr;
	if (!PhysScene || DeltaTime <= 0.0f)
	{
		return;
	}

	bool bHasCloth = false;
	for (int32 i = 0; i < Layers.Num(); ++i)
	{
		if (Layers[i]->ClothComponent && Layers[i]->bVisible && Layers[i]->ClothComponent->bEnableClothSimulation)
		{
			bHasCloth = true;
			break;
		}
	}
	if (!bHasCloth)
	{
		return;
	}

	FPhysicsSettings Settings{};
	PhysScene->SetUpForFrame(std::min(DeltaTime, 1.0f / 30.0f), Settings);
	PhysScene->StartFrame();
	PhysScene->EndFrame();
}

void FClothingEditor::OnRenderLine(FLineBatcher& LineBatcher)
{
	FSkeletalPreviewEditor::OnRenderLine(LineBatcher);
	DrawLayerDebugOverlays(LineBatcher);
}

void FClothingEditor::DrawLayerDebugOverlays(FLineBatcher& LineBatcher) const
{
	const FClothingLayer* ActiveLayer = GetSelectedLayer();

	// 선택 스타킹 영역 와이어프레임 표시
	if (ActiveLayer && ActiveLayer->Kind == EClothingLayerKind::Stocking && ActiveLayer->StockingRegion &&
		(bShowRegionWireframe || ViewportTool == EClothingViewportTool::PaintRegion) && Mesh && MeshComponent)
	{
		FSkeletalMeshObject* MeshObject = MeshComponent->GetMeshObject();
		const TArray<FVertex>* Vertices = MeshObject ? MeshObject->GetCPUSkinnedVertices() : nullptr;
		if (Vertices && !Mesh->GetRenderData().LODRenderData.IsEmpty())
		{
			const TArray<uint32>& Indices = Mesh->GetRenderData().LODRenderData[0].Indices;
			const int32 TriCount = Indices.Num() / 3;
			const FMatrix WorldMat = MeshComponent->GetWorldMatrix();
			const FVector4 Color(0.15f, 0.95f, 0.85f, 1.0f);
			const TArray<uint32>& Tris = ActiveLayer->StockingRegion->GetTriangleIndices();
			for (int32 i = 0; i < Tris.Num(); ++i)
			{
				const uint32 Tri = Tris[i];
				if (static_cast<int32>(Tri) >= TriCount)
				{
					continue;
				}
				const FVertex& V0 = (*Vertices)[Indices[Tri * 3]];
				const FVertex& V1 = (*Vertices)[Indices[Tri * 3 + 1]];
				const FVertex& V2 = (*Vertices)[Indices[Tri * 3 + 2]];
				const FVector P0 = WorldMat.TransformPosition(V0.Position + V0.Normal * 0.003f);
				const FVector P1 = WorldMat.TransformPosition(V1.Position + V1.Normal * 0.003f);
				const FVector P2 = WorldMat.TransformPosition(V2.Position + V2.Normal * 0.003f);
				LineBatcher.AddLine(P0, P1, Color);
				LineBatcher.AddLine(P1, P2, Color);
				LineBatcher.AddLine(P2, P0, Color);
			}
		}
	}

	// 마우스 집어 당기기 앵커와 드래그 선 표시
	if (InteractionComponent && InteractionComponent->IsGrabbing())
	{
		UStockingComponent* Pulled = InteractionComponent->GetActiveStocking();
		if (Pulled)
		{
			const FVector Anchor = Pulled->GetAnchorWorldPosition();
			const FVector Target = InteractionComponent->GetCurrentDragWorldPosition();
			LineBatcher.AddSphere(Anchor, 0.012f, FVector4(1.0f, 0.85f, 0.1f, 1.0f));
			LineBatcher.AddSphere(Target, 0.015f, FVector4(1.0f, 0.25f, 0.35f, 1.0f));
			LineBatcher.AddLine(Anchor, Target, FVector4(1.0f, 0.55f, 0.2f, 1.0f));
			LineBatcher.AddSphere(Anchor, Pulled->PullRadius, FVector4(0.3f, 0.85f, 1.0f, 0.6f), 20);
		}
	}

	// 천 캡슐 콜라이더 디버그 시각화
	if (bShowColliders && ActiveLayer && ActiveLayer->ClothComponent && MeshComponent)
	{
		const FVector4 CapColor(0.25f, 0.9f, 0.4f, 0.85f);
		for (int32 i = 0; i < ActiveLayer->BoneCapsules.Num(); ++i)
		{
			const FClothBoneCapsule& Cap = ActiveLayer->BoneCapsules[i];
			const int32 IdxA = MeshComponent->FindBoneIndex(Cap.BoneA);
			const int32 IdxB = MeshComponent->FindBoneIndex(Cap.BoneB);
			if (IdxA < 0 || IdxB < 0)
			{
				continue;
			}
			const FVector PosA = MeshComponent->GetBoneWorldMatrix(IdxA).TransformPosition(FVector::ZeroVector);
			const FVector PosB = MeshComponent->GetBoneWorldMatrix(IdxB).TransformPosition(FVector::ZeroVector);
			LineBatcher.AddSphere(PosA, Cap.RadiusA, CapColor, 12);
			LineBatcher.AddSphere(PosB, Cap.RadiusB, CapColor, 12);
			LineBatcher.AddLine(PosA, PosB, CapColor);
		}
	}

	// 의상 컴포넌트 로컬 바운드 시각화
	if (bShowBounds && ActiveLayer)
	{
		if (ActiveLayer->StockingComponent)
		{
			LineBatcher.AddOrientedBox(
				ActiveLayer->StockingComponent->CalcLocalBounds(),
				ActiveLayer->StockingComponent->GetWorldMatrix(),
				FVector4(1.0f, 0.7f, 0.2f, 1.0f));
		}
		if (ActiveLayer->ClothComponent)
		{
			LineBatcher.AddOrientedBox(
				ActiveLayer->ClothComponent->CalcLocalBounds(),
				ActiveLayer->ClothComponent->GetWorldMatrix(),
				FVector4(0.3f, 0.95f, 0.5f, 1.0f));
		}
	}
}

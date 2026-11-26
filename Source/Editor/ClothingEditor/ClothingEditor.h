#pragma once

#include "Editor/SkeletalPreviewEditor/SkeletalPreviewEditor.h"
#include "Editor/Gizmo/Gizmo.h"
#include "Components/ClothComponent.h"
#include "Render/StockingMaterial.h"

class UStockingRegion;
class UStockingComponent;
class UStockingInteractionComponent;
class UMaterialInstanceDynamic;
class UAnimSequence;

enum class EClothingLayerKind : uint8
{
	Stocking,
	ClothCape,
	ClothSkirt,
	ClothStaticMesh
};

enum class EClothingViewportTool : uint8
{
	PullTest,
	TearBrush,
	PaintRegion,
	PickBone
};

struct FClothingLayer
{
	FString Name;
	EClothingLayerKind Kind = EClothingLayerKind::Stocking;
	bool bVisible = true;

	// 스타킹 레이어 자원
	UStockingRegion* StockingRegion = nullptr;
	UStockingComponent* StockingComponent = nullptr;
	FStockingMaterialParams StockingMatParams;
	UMaterialInstanceDynamic* StockingMaterialInstance = nullptr;
	FString LastStatusMessage;
	bool bRegionValid = false;

	// 천 물리 레이어 자원
	UClothComponent* ClothComponent = nullptr;
	UMaterialInstanceDynamic* ClothMaterialInstance = nullptr;
	FVector4 ClothColor = FVector4(0.55f, 0.12f, 0.16f, 1.0f);
	FVector ClothLocalOffset = FVector::ZeroVector;
	FString PinBoneName;
	TArray<FClothBoneCapsule> BoneCapsules;
	FString StaticMeshAssetPath;
};

class FClothingEditor : public FSkeletalPreviewEditor
{
public:
	FClothingEditor(
		UEditorEngine& InEditorEngine,
		FEditorManager& InEditorManager,
		const FString& InDocumentKey,
		const FString& InMeshAssetPath,
		USkeletalMesh& InMesh,
		const FString& InInitialRegionFile = FString());

	const char* GetEditorTypeName() const override { return "Clothing"; }
	bool UsesPreviewSkybox() const override { return true; }
	const FGizmo* GetPreviewGizmo() const override;
	void OnRenderLine(FLineBatcher& LineBatcher) override;

	// 의상 레이어 생성 및 관리
	int32 AddStockingLayer(const FString& InName, bool bAutoSelectLegs, const FString& RegionFilePath = FString());
	int32 AddCapeClothLayer(const FString& InName);
	int32 AddSkirtClothLayer(const FString& InName);
	int32 AddStaticMeshClothLayer(const FString& InName);
	void RemoveLayer(int32 LayerIndex);
	void SelectLayer(int32 LayerIndex);
	void SetLayerVisible(int32 LayerIndex, bool bVisible);

	int32 GetLayerCount() const { return Layers.Num(); }
	int32 GetSelectedLayerIndex() const { return SelectedLayerIndex; }
	FClothingLayer* GetLayer(int32 LayerIndex);
	const FClothingLayer* GetLayer(int32 LayerIndex) const;
	FClothingLayer* GetSelectedLayer() { return GetLayer(SelectedLayerIndex); }
	const FClothingLayer* GetSelectedLayer() const { return GetLayer(SelectedLayerIndex); }

	// 스타킹 레이어 조작
	bool RebuildStockingLayer(int32 LayerIndex, FString& OutMessage);
	bool ValidateStockingLayer(int32 LayerIndex, FString& OutMessage);
	bool SaveStockingLayerRegion(int32 LayerIndex, FString& OutMessage);
	bool LoadStockingLayerRegion(int32 LayerIndex, const FString& FilePath, FString& OutMessage);
	void ApplyStockingMaterialParams(int32 LayerIndex);
	void SelectStockingTrianglesByBone(int32 LayerIndex, int32 BoneIndex, float MinWeightThreshold = 0.25f);
	void AutoSelectLegTriangles(int32 LayerIndex);
	void GrowStockingLayerRegion(int32 LayerIndex);
	void ShrinkStockingLayerRegion(int32 LayerIndex);
	void ClearStockingLayerRegion(int32 LayerIndex);
	void RepairAllStockingTears();

	// 천 물리 레이어 조작
	void RebuildClothLayer(int32 LayerIndex);
	void ApplyClothLayerParams(int32 LayerIndex, const char* ChangedPropertyName = "StretchStiffness");
	void ApplyClothLayerColor(int32 LayerIndex);
	void ResetAllClothPoses();
	void PopulateDefaultBodyCapsules(FClothingLayer& Layer, bool bUpperBody);

	// 뷰포트 도구 및 디버그 표시
	void SetViewportTool(EClothingViewportTool InTool);
	EClothingViewportTool GetViewportTool() const { return ViewportTool; }
	EGizmoMode GetGizmoMode() const { return BoneGizmo.GetMode(); }
	void SetGizmoMode(EGizmoMode Mode);
	bool IsGizmoDragging() const { return BoneGizmo.IsUsing(); }
	void SetRegionEraseMode(bool bErase) { bRegionEraseMode = bErase; }
	bool IsRegionEraseMode() const { return bRegionEraseMode; }
	void SetRegionBrushRadius(float Radius) { RegionBrushRadius = FMath::Clamp(Radius, 0.0f, CharacterScale * 0.6f); }
	float GetRegionBrushRadius() const { return RegionBrushRadius; }
	float GetCharacterScale() const { return CharacterScale; }
	void SetAutoLegMaxRadiusRatio(float Ratio) { AutoLegMaxRadiusRatio = FMath::Clamp(Ratio, 0.02f, 0.30f); }
	float GetAutoLegMaxRadiusRatio() const { return AutoLegMaxRadiusRatio; }
	void SetAutoLegMaxHeightRatio(float Ratio) { AutoLegMaxHeightRatio = FMath::Clamp(Ratio, 0.15f, 1.25f); }
	float GetAutoLegMaxHeightRatio() const { return AutoLegMaxHeightRatio; }

	void SetSimulateClothPhysics(bool bEnable) { bSimulateClothPhysics = bEnable; }
	bool IsSimulatingClothPhysics() const { return bSimulateClothPhysics; }
	void SetShowRegionWireframe(bool bShow) { bShowRegionWireframe = bShow; }
	bool IsShowingRegionWireframe() const { return bShowRegionWireframe; }
	void SetShowColliders(bool bShow) { bShowColliders = bShow; }
	bool IsShowingColliders() const { return bShowColliders; }
	void SetShowBounds(bool bShow) { bShowBounds = bShow; }
	bool IsShowingBounds() const { return bShowBounds; }

	// 애니메이션 및 수동 본 포즈 확인
	bool PlayAnimationByPath(const FString& AnimPath);
	void ToggleAnimationPlayback();
	void StopAnimation();
	bool IsAnimationPlaying() const;
	bool IsAnimationLooping() const { return bAnimLooping; }
	void SetAnimationLooping(bool bLoop);
	const FString& GetCurrentAnimationPath() const { return CurrentAnimationPath; }
	const FString& GetMeshAssetPath() const { return MeshAssetPath; }
	bool GetSelectedBoneLocalTransform(FTransform& OutTransform) const;
	void SetSelectedBoneLocalTransform(const FTransform& Transform);
	void ResetCharacterPose();

protected:
	TUniquePtr<IEditorTab> CreateTab() override;
	bool InitializeResources() override;
	void ShutdownResources() override;
	void TickBeforeWorld(float DeltaTime) override;
	void TickAfterWorld(float DeltaTime) override;
	bool IsViewportInteractionBusy() const override;
	void OnBoneSelected() override;
	void CancelViewportInteraction() override;

private:
	void UpdatePullInteraction();
	void UpdateTearBrushInput();
	void UpdateRegionBrushInput();
	void SyncBoneGizmo();
	bool UpdateBoneGizmo();
	void ApplyBoneGizmoTransform();
	FQuat GetBoneWorldRotation(int32 BoneIndex) const;
	void SyncAllStockingsToCharacter();
	void StepPreviewClothPhysics(float DeltaTime);
	void DrawLayerDebugOverlays(FLineBatcher& LineBatcher) const;
	FString FindFirstMatchingBone(const TArray<const char*>& Keywords) const;
	FString ResolveDefaultRegionFilePath() const;

	FString MeshAssetPath;
	FString InitialRegionFile;
	FString CurrentAnimationPath;
	UAnimSequence* CurrentAnimation = nullptr;
	bool bAnimLooping = true;

	UStockingInteractionComponent* InteractionComponent = nullptr;
	USceneComponent* BoneGizmoTarget = nullptr;
	FGizmo BoneGizmo;
	FTransform DragStartBoneTransform;
	FMatrix DragParentInverse;
	FQuat DragParentRotation;
	TArray<TUniquePtr<FClothingLayer>> Layers;
	int32 SelectedLayerIndex = -1;

	EClothingViewportTool ViewportTool = EClothingViewportTool::PullTest;
	bool bRegionEraseMode = false;
	bool bHasLastTearHit = false;
	FVector LastTearHitLocal = FVector::ZeroVector;
	float CharacterScale = 1.0f;
	float RegionBrushRadius = 0.06f;
	float AutoLegMaxRadiusRatio = 0.072f;
	float AutoLegMaxHeightRatio = 0.84f;
	bool bSimulateClothPhysics = true;
	bool bShowRegionWireframe = false;
	bool bShowColliders = true;
	bool bShowBounds = false;
};

#pragma once

#include "Editor/SkeletalPreviewEditor/SkeletalPreviewEditor.h"
#include "Physics/AggregateGeom.h"
#include "Editor/Gizmo/Gizmo.h"

class UBodySetup;
class UPhysicsAsset;
struct FPhysicsAssetConstraint;
class FPhysicsAssetSimulation;
class UStaticMeshComponent;

// 에셋의 BodySetup을 참조하는 에디터 뷰입니다. 형상 데이터는 복사하지 않습니다.
struct FPhysicsAssetPreviewBody
{
	FPhysicsAssetPreviewBody(int32 InBoneIndex, UBodySetup* InBodySetup)
		: BoneIndex(InBoneIndex), BodySetup(InBodySetup) {}
	int32 BoneIndex;
	UBodySetup* BodySetup;
	FAggregateGeom& GetGeometry();
	const FAggregateGeom& GetGeometry() const;
};

// 피직스 에셋 편집에 사용할 스켈레탈 메시 미리보기 에디터입니다.
class FPhysicsAssetEditor : public FSkeletalPreviewEditor
{
public:
	enum class EPreviewShapeType { None, Sphere, Box, Capsule };
	enum class EBodyDrawMode { All, SelectedOnly, None };
	enum class ESimulationMode { All, SelectedAndBelow };

	FPhysicsAssetEditor(
		UEditorEngine& InEditorEngine,
		FEditorManager& InEditorManager,
		const FString& InAssetPath,
		USkeletalMesh& InPreviewMesh
	);
	~FPhysicsAssetEditor() override;
	bool Save();
	bool IsDirty() const override { return bDirty; }
	bool ConfirmClose() override;
	const FString& GetSaveMessage() const { return SaveMessage; }
	bool HasSaveError() const { return bSaveFailed; }

	void OnRenderLine(FLineBatcher& LineBatcher) override;
	void OnRenderDebugOverlay(const FSceneView* View) override;
	const FGizmo* GetPreviewGizmo() const override { return !IsSimulationActive() && ShapeGizmo.GetTarget() && IsBodyVisible(SelectedBodyIndex) ? &ShapeGizmo : nullptr; }
	bool StartSimulation();
	void StopSimulation();
	void ToggleSimulation();
	bool IsSimulationActive() const;
	bool CanStartSimulation() const;
	void SetSimulationMode(ESimulationMode Mode);
	ESimulationMode GetSimulationMode() const { return SimulationMode; }
	const FString& GetSimulationError() const { return SimulationError; }
	void SetBodyDrawMode(EBodyDrawMode Mode);
	EBodyDrawMode GetBodyDrawMode() const { return BodyDrawMode; }
	void SetBodyColor(const FVector4& Color) { BodyColor = Color; }
	const FVector4& GetBodyColor() const { return BodyColor; }
	void SetSelectedBodyColor(const FVector4& Color) { SelectedBodyColor = Color; }
	const FVector4& GetSelectedBodyColor() const { return SelectedBodyColor; }
	void SetSelectedShapeColor(const FVector4& Color) { SelectedShapeColor = Color; }
	const FVector4& GetSelectedShapeColor() const { return SelectedShapeColor; }
	void SetBodyDepthTest(bool bEnable) { bBodyDepthTest = bEnable; }
	bool IsBodyDepthTestEnabled() const { return bBodyDepthTest; }
	void SetGizmoMode(EGizmoMode Mode);
	EGizmoMode GetGizmoMode() const { return ShapeGizmo.GetMode(); }
	bool IsGizmoDragging() const { return ShapeGizmo.IsUsing(); }
	int32 AddBody(int32 BoneIndex, EPreviewShapeType ShapeType = EPreviewShapeType::Capsule);
	bool RemoveBody(int32 BodyIndex);
	bool GenerateBodies();
	int32 AddShape(int32 BodyIndex, EPreviewShapeType ShapeType);
	bool RemoveShape(int32 BodyIndex, EPreviewShapeType ShapeType, int32 ShapeIndex);
	bool SelectShape(int32 BodyIndex, EPreviewShapeType ShapeType, int32 ShapeIndex);
	int32 FindBodyByBone(int32 BoneIndex) const;
	void SelectBody(int32 BodyIndex);
	int32 GetSelectedBodyIndex() const { return SelectedBodyIndex; }
	const FPhysicsAssetPreviewBody* GetSelectedBody() const;
	const TArray<FPhysicsAssetPreviewBody>& GetPreviewBodies() const { return PreviewBodies; }
	UPhysicsAsset* GetPhysicsAsset() const { return PhysicsAsset; }
	int32 GetNearestParentBody(int32 ChildBodyIndex) const;
	int32 FindConstraintForBody(int32 ChildBodyIndex) const;
	bool CanAddConstraint(int32 ParentBodyIndex, int32 ChildBodyIndex) const;
	int32 AddConstraint(int32 ParentBodyIndex, int32 ChildBodyIndex);
	bool RemoveConstraint(int32 ConstraintIndex);
	void SelectConstraint(int32 ConstraintIndex);
	int32 GetSelectedConstraintIndex() const { return SelectedConstraintIndex; }
	const FPhysicsAssetConstraint* GetSelectedConstraint() const;
	bool SetSelectedConstraintProperties(const FPhysicsAssetConstraint& Properties);
	int32 GetSelectedShapeIndex() const { return SelectedShapeIndex; }
	EPreviewShapeType GetSelectedShapeType() const { return SelectedShapeType; }
	uint32 GetTopologyRevision() const { return TopologyRevision; }
	// Size: Sphere=(반지름,0,0), Box=(X,Y,Z 전체 길이), Capsule=(반지름,원통 길이,0).
	bool GetSelectedShapeProperties(FShapeElem& OutShape, FVector& OutSize, EPreviewShapeType& OutType) const;
	bool SetSelectedShapeProperties(const FShapeElem& Shape, const FVector& Size);

	const char* GetEditorTypeName() const override { return "PhysicsAsset"; }
	bool UsesPreviewSkybox() const override { return true; }

protected:
	TUniquePtr<IEditorTab> CreateTab() override;
	bool InitializeResources() override;
	void ShutdownResources() override;
	void TickAfterWorld(float DeltaTime) override;
	bool IsViewportInteractionBusy() const override { return ShapeGizmo.IsUsing(); }
	void OnBoneSelected() override;
	void CancelViewportInteraction() override;

private:
	void MarkModified();
	void RebuildBodyViews();
	bool InitializePreviewFloor();
	bool DiscardChanges();
	TArray<uint8> SavedAssetBytes;
	FString SaveMessage;
	bool bDirty = false;
	bool bSaveFailed = false;
	bool bDirtyBeforeGizmo = false;
	TUniquePtr<FPhysicsAssetSimulation> Simulation;
	UStaticMeshComponent* PreviewFloor = nullptr;
	FString SimulationError;
	ESimulationMode SimulationMode = ESimulationMode::All;
	FShapeElem* GetSelectedShape();
	const FShapeElem* GetSelectedShape() const;
	FVector GetSelectedShapeSize() const;
	void SetSelectedShapeSize(const FVector& Size);
	bool UpdateShapePicking();
	bool UpdateShapeGizmo();
	void SyncShapeGizmo();
	void ApplyShapeGizmoTransform();
	FQuat GetPreviewBoneWorldRotation() const;
	bool IsBodyVisible(int32 BodyIndex) const;

	EBodyDrawMode BodyDrawMode = EBodyDrawMode::All;
	FVector4 BodyColor = FVector4(0.2f, 1.0f, 0.4f, 1.0f);
	FVector4 SelectedBodyColor = FVector4(1.0f, 0.55f, 0.1f, 1.0f);
	FVector4 SelectedShapeColor = FVector4(1.0f, 0.8f, 0.1f, 1.0f);
	bool bBodyDepthTest = false;
	TUniquePtr<FLineBatcher> PhysicsBodyLines;

	// 각 바디는 생성된 본을 유지하며, 본 선택과 바디 선택은 별도로 관리합니다.
	TArray<FPhysicsAssetPreviewBody> PreviewBodies;
	UPhysicsAsset* PhysicsAsset = nullptr; // 이 문서만 소유하는 편집본. 공유 메시의 기본 Physics Asset은 변경하지 않습니다.
	uint32 TopologyRevision = 0;
	int32 SelectedBodyIndex = -1;
	int32 SelectedConstraintIndex = -1;
	EPreviewShapeType SelectedShapeType = EPreviewShapeType::None;
	int32 SelectedShapeIndex = -1;
	USceneComponent* ShapeGizmoTarget = nullptr;
	FGizmo ShapeGizmo;
	FShapeElem DragStartShape;
	FVector DragStartSize = FVector::ZeroVector;
	FMatrix DragBoneInverse;
	FQuat DragBoneRotation = FQuat::Identity();
	int32 DragScaleAxis = -1;
};

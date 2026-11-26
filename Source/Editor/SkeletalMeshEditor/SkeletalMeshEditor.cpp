#include "EnginePCH.h"
#include "Editor/SkeletalMeshEditor/SkeletalMeshEditor.h"
#include "Editor/AssetEditor/EditorManager.h"
#include "SkeletalMeshEditorTab.h"
#include "Components/SkeletalMeshComponent.h"
#include "Render/SkeletalMesh.h"
#include "Render/SkeletalMeshObject.h"
#include "Render/StockingRegion.h"
#include "Render/LineBatcher.h"
#include "Collision/Ray.h"
#include "Asset/AssetManager.h"
#include "Asset/AssetUtils.h"
#include "Physics/BodySetup.h"
#include "Physics/PhysicsAsset.h"
#include "Physics/PhysicsAssetFile.h"
#include "PhysicsAssetUtils.h"
#include "Serialization/JsonArchive.h"
#include "UObject/UObjectGlobals.h"
#include <filesystem>

bool FSkeletalMeshEditor::InitializeResources()
{
	if (!InitializePreview()) return false;
	BoneGizmoTarget = Cast<USceneComponent>(PreviewScene.AddComponent(USceneComponent::StaticClass()));
	if (!BoneGizmoTarget) return false;
	BoneGizmoTarget->DetachFromParent();
	ResetBonePoseHistory();

	EditingStockingRegion = NewObject<UStockingRegion>();
	if (EditingStockingRegion && Mesh)
	{
		EditingStockingRegion->BindToSourceMesh(*Mesh, GetAssetPath(), 0);
		const FString RegionFile = ResolveDefaultRegionFilePath();
		if (!RegionFile.empty() && std::filesystem::exists(RegionFile))
		{
			FString StatusMsg;
			LoadStockingRegion(StatusMsg);
		}
		else
		{
			StockingRegionStatus = "No saved stocking region. Select triangles to define one.";
			bStockingRegionValid = false;
		}
	}
	return true;
}

void FSkeletalMeshEditor::ShutdownResources()
{
	EditingStockingRegion = nullptr;
	bRegionSelectMode = false;
	PoseUndoStack.Empty();
	PoseRedoStack.Empty();
	CommittedBonePose.Empty();
	bPoseUndoPending = false;
	BoneGizmo.EndDrag();
	BoneGizmo.SetTarget(nullptr);
	BoneGizmoTarget = nullptr;
	FSkeletalPreviewEditor::ShutdownResources();
}

void FSkeletalMeshEditor::TickAfterWorld(float DeltaTime)
{
	if (bRegionSelectMode)
	{
		BoneGizmo.EndDrag();
		UpdateRegionTrianglePicking();
		return;
	}

	// 기즈모가 입력을 쓰지 않았을 때만 본을 피킹하고, 마지막에 기즈모를 선택한 본 위치로 맞춘다
	if (!UpdateBoneGizmo())
		UpdateBonePicking();
	SyncBoneGizmo();
}

TUniquePtr<IEditorTab> FSkeletalMeshEditor::CreateTab()
{
	return MakeUnique<FSkeletalMeshEditorTab>(*this);
}

void FSkeletalMeshEditor::SetGizmoMode(EGizmoMode Mode)
{
	BoneGizmo.EndDrag();
	BoneGizmo.SetMode(Mode);
	SyncBoneGizmo();
}

void FSkeletalMeshEditor::SetGizmoSpace(EGizmoSpace Space)
{
	BoneGizmo.EndDrag();
	BoneGizmo.SetSpace(Space);
	SyncBoneGizmo();
}

void FSkeletalMeshEditor::OpenAnimationAsset(const FString& InAssetPath)
{
	if (!InAssetPath.empty())
	{
		EditorManager.OpenEditor(InAssetPath);
	}
}

void FSkeletalMeshEditor::OpenInClothingEditor()
{
	if (!GetAssetPath().empty())
	{
		EditorManager.OpenClothingEditor(GetAssetPath());
	}
}

void FSkeletalMeshEditor::SetRegionSelectMode(bool bEnable)
{
	bRegionSelectMode = bEnable;
	if (bEnable)
	{
		BoneGizmo.EndDrag();
		BoneGizmo.SetTarget(nullptr);
	}
	else
	{
		SyncBoneGizmo();
	}
}

FString FSkeletalMeshEditor::ResolveDefaultRegionFilePath() const
{
	const FString MeshFile = FAssetUtils::ResolveAssetFilePath(GetAssetPath());
	return MeshFile.empty() ? FString() : (MeshFile + ".stockingregion");
}

void FSkeletalMeshEditor::SelectRegionBySelectedBone(float MinWeightThreshold)
{
	if (!EditingStockingRegion || !Mesh || SelectedBoneIndex < 0)
	{
		return;
	}
	TArray<int32> Bones;
	Bones.Add(SelectedBoneIndex);
	EditingStockingRegion->SelectTrianglesByBones(*Mesh, Bones, MinWeightThreshold);
	FString Status;
	ValidateStockingRegion(Status);
}

void FSkeletalMeshEditor::GrowStockingRegion()
{
	if (!EditingStockingRegion || !Mesh)
	{
		return;
	}
	EditingStockingRegion->GrowSelection(*Mesh);
	FString Status;
	ValidateStockingRegion(Status);
}

void FSkeletalMeshEditor::ShrinkStockingRegion()
{
	if (!EditingStockingRegion || !Mesh)
	{
		return;
	}
	EditingStockingRegion->ShrinkSelection(*Mesh);
	FString Status;
	ValidateStockingRegion(Status);
}

void FSkeletalMeshEditor::ClearStockingRegion()
{
	if (!EditingStockingRegion)
	{
		return;
	}
	EditingStockingRegion->ClearTriangles();
	bStockingRegionValid = false;
	StockingRegionStatus = "Region selection cleared.";
}

bool FSkeletalMeshEditor::SaveStockingRegion(FString& OutMessage)
{
	OutMessage.clear();
	if (!EditingStockingRegion || !Mesh)
	{
		OutMessage = "No stocking region or mesh available.";
		StockingRegionStatus = OutMessage;
		return false;
	}

	EditingStockingRegion->BindToSourceMesh(*Mesh, GetAssetPath(), 0);
	const FString FilePath = ResolveDefaultRegionFilePath();
	if (FilePath.empty() || !EditingStockingRegion->SaveToFile(FilePath))
	{
		OutMessage = "Failed to save stocking region file.";
		StockingRegionStatus = OutMessage;
		return false;
	}

	EditingStockingRegion->SetRegionFilePath(FilePath);
	FString ValidMsg;
	ValidateStockingRegion(ValidMsg);
	OutMessage = "Saved region (" + std::to_string(EditingStockingRegion->GetSelectedTriangleCount()) + " tris).";
	StockingRegionStatus = OutMessage;
	return true;
}

bool FSkeletalMeshEditor::LoadStockingRegion(FString& OutMessage)
{
	OutMessage.clear();
	if (!EditingStockingRegion || !Mesh)
	{
		OutMessage = "No stocking region or mesh available.";
		StockingRegionStatus = OutMessage;
		return false;
	}

	const FString FilePath = ResolveDefaultRegionFilePath();
	FString Error;
	if (!EditingStockingRegion->LoadFromFile(FilePath, Error))
	{
		OutMessage = Error;
		StockingRegionStatus = Error;
		bStockingRegionValid = false;
		return false;
	}

	return ValidateStockingRegion(OutMessage);
}

bool FSkeletalMeshEditor::ValidateStockingRegion(FString& OutMessage)
{
	OutMessage.clear();
	if (!EditingStockingRegion || !Mesh)
	{
		bStockingRegionValid = false;
		OutMessage = "Mesh or region is missing.";
		StockingRegionStatus = OutMessage;
		return false;
	}

	FString Error;
	bStockingRegionValid = EditingStockingRegion->ValidateAgainstMesh(*Mesh, Error);
	if (bStockingRegionValid)
	{
		OutMessage = "Valid region (" + std::to_string(EditingStockingRegion->GetSelectedTriangleCount()) + " triangles).";
	}
	else
	{
		OutMessage = Error;
	}
	StockingRegionStatus = OutMessage;
	return bStockingRegionValid;
}

void FSkeletalMeshEditor::UpdateRegionTrianglePicking()
{
	if (!CanProcessViewportInput() || !EditingStockingRegion || !Mesh || !MeshComponent ||
		!PointerInput.bHovered || !PointerInput.bMouseDown || CameraInput.bCaptured ||
		ViewportWidth == 0 || ViewportHeight == 0)
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

	const FVector HitLocal = LocalRay.PointAt(BestT);
	const float RadiusSq = RegionBrushRadius * RegionBrushRadius;
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
				EditingStockingRegion->RemoveTriangle(static_cast<uint32>(t));
			}
			else
			{
				EditingStockingRegion->AddTriangle(static_cast<uint32>(t));
			}
		}
	}

	FString Status;
	ValidateStockingRegion(Status);
}

void FSkeletalMeshEditor::OnRenderLine(FLineBatcher& LineBatcher)
{
	FSkeletalPreviewEditor::OnRenderLine(LineBatcher);
	RenderSelectedRegionTriangles(LineBatcher);
}

void FSkeletalMeshEditor::RenderSelectedRegionTriangles(FLineBatcher& LineBatcher) const
{
	if (!EditingStockingRegion || !Mesh || !MeshComponent || EditingStockingRegion->GetSelectedTriangleCount() == 0)
	{
		return;
	}

	FSkeletalMeshObject* MeshObject = MeshComponent->GetMeshObject();
	const TArray<FVertex>* Vertices = MeshObject ? MeshObject->GetCPUSkinnedVertices() : nullptr;
	if (!Vertices || Mesh->GetRenderData().LODRenderData.IsEmpty())
	{
		return;
	}

	const TArray<uint32>& Indices = Mesh->GetRenderData().LODRenderData[0].Indices;
	const int32 TriCount = Indices.Num() / 3;
	const FMatrix WorldMat = MeshComponent->GetWorldMatrix();
	const FVector4 LineColor = bRegionSelectMode
		? FVector4(0.15f, 0.95f, 0.85f, 1.0f)
		: FVector4(0.85f, 0.35f, 0.95f, 0.8f);

	const TArray<uint32>& SelectedTris = EditingStockingRegion->GetTriangleIndices();
	for (int32 i = 0; i < SelectedTris.Num(); ++i)
	{
		const uint32 Tri = SelectedTris[i];
		if (static_cast<int32>(Tri) >= TriCount)
		{
			continue;
		}
		const FVertex& V0 = (*Vertices)[Indices[Tri * 3]];
		const FVertex& V1 = (*Vertices)[Indices[Tri * 3 + 1]];
		const FVertex& V2 = (*Vertices)[Indices[Tri * 3 + 2]];

		const FVector P0 = WorldMat.TransformPosition(V0.Position + V0.Normal * 0.002f);
		const FVector P1 = WorldMat.TransformPosition(V1.Position + V1.Normal * 0.002f);
		const FVector P2 = WorldMat.TransformPosition(V2.Position + V2.Normal * 0.002f);

		LineBatcher.AddLine(P0, P1, LineColor);
		LineBatcher.AddLine(P1, P2, LineColor);
		LineBatcher.AddLine(P2, P0, LineColor);
	}
}

bool FSkeletalMeshEditor::CreatePhysicsAsset(const FPhysAssetCreateParams& Params, FString& OutError)
{
	OutError.clear();
	if (!Mesh)
	{
		OutError = "No skeletal mesh is open";
		return false;
	}
	const FString MeshFile = FAssetUtils::ResolveAssetFilePath(GetAssetPath());
	if (MeshFile.empty())
	{
		OutError = "Cannot resolve the skeletal mesh file";
		return false;
	}
	const std::filesystem::path AssetFile = std::filesystem::path(MeshFile + ".physics.json");
	const bool bExists = std::filesystem::exists(AssetFile);
	std::filesystem::path OutputFile = AssetFile;
	if (bExists)
	{
		int32 Suffix = 0;
		do
		{
			OutputFile = AssetFile.string() + ".generated" + (Suffix ? std::to_string(Suffix) : FString());
			++Suffix;
		} while (std::filesystem::exists(OutputFile));
	}
	UPhysicsAsset* Asset = NewObject<UPhysicsAsset>();
	Asset->PreviewMeshPath = FAssetUtils::GetAssetKey(MeshFile);
	const bool bGenerated = FPhysicsAssetUtils::CreateFromSkeletalMesh(*Mesh, *Asset, Params, OutError);
	TArray<uint8> Bytes;
	bool bSaved = bGenerated && FJsonArchive::SaveObjectToBytes(Asset, Bytes);
	if (bSaved)
	{
		std::ofstream OutStream(OutputFile, std::ios::binary | std::ios::trunc);
		bSaved = static_cast<bool>(OutStream) &&
			static_cast<bool>(OutStream.write(reinterpret_cast<const char*>(Bytes.GetData()), static_cast<std::streamsize>(Bytes.Num())));
		if (!bSaved)
		{
			OutError = "Could not write Physics Asset file: " + OutputFile.string();
		}
	}
	else if (bGenerated)
	{
		OutError = "Could not serialize the generated Physics Asset";
	}
	auto DiscardAsset = [&]
	{
		Asset->ClearBodies();
		delete Asset;
	};
	if (!bSaved) { DiscardAsset(); return false; }
	if (bExists)
	{
		std::filesystem::path BackupFile;
		int32 Suffix = 0;
		do
		{
			BackupFile = AssetFile.string() + ".bak" + (Suffix ? std::to_string(Suffix) : FString());
			++Suffix;
		} while (std::filesystem::exists(BackupFile));
		std::error_code Error;
		if (!std::filesystem::copy_file(AssetFile, BackupFile, std::filesystem::copy_options::none, Error))
		{
			std::filesystem::remove(OutputFile);
			DiscardAsset();
			OutError = "Could not back up the existing Physics Asset";
			return false;
		}
		std::filesystem::remove(AssetFile, Error);
		if (!Error) std::filesystem::rename(OutputFile, AssetFile, Error);
		if (Error)
		{
			std::filesystem::copy_file(BackupFile, AssetFile, std::filesystem::copy_options::none);
			std::filesystem::remove(OutputFile);
			DiscardAsset();
			OutError = "Could not replace Physics Asset; backup saved at " + BackupFile.string();
			return false;
		}
		UAssetManager::RegisterPhysicsAsset(AssetFile.string(), Asset);
		Mesh->SetPhysicsAsset(Asset);
		OutError = "Physics Asset regenerated. Backup: " + BackupFile.string();
		return true;
	}
	UAssetManager::RegisterPhysicsAsset(AssetFile.string(), Asset);
	Mesh->SetPhysicsAsset(Asset);
	return true;
}

void FSkeletalMeshEditor::SyncBoneGizmo()
{
	if (!MeshComponent || !BoneGizmoTarget || SelectedBoneIndex < 0 || SelectedBoneIndex >= BoneItems.Num() ||
		BoneDrawMode == EBoneDrawMode::None)
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

	// 스케일은 조작 배율로만 사용합니다. 부모의 비균일 스케일과 전단을 축 회전에 섞지 않습니다.
	BoneGizmoTarget->SetTransform(FTransform(Rotation, Location));
	BoneGizmo.SetTarget(BoneGizmoTarget);
}

FQuat FSkeletalMeshEditor::GetBoneWorldRotation(int32 BoneIndex) const
{
	FQuat Rotation = FQuat::Identity();
	const TArray<FBoneInfo>& Bones = Mesh->GetRefSkeleton();
	while (BoneIndex >= 0)
	{
		Rotation = MeshComponent->GetBoneLocalTransform(BoneIndex).Rotation * Rotation;
		BoneIndex = Bones[BoneIndex].ParentIndex;
	}

	return (MeshComponent->GetWorldRotation().Quaternion() * Rotation).Normalized();
}

bool FSkeletalMeshEditor::UpdateBoneGizmo()
{
	if
		(
			// 공통 뷰포트 입력 조건을 만족하지 않으면 입력을 무시
			!CanProcessViewportInput() ||

			// BoneGizmo의 대상이 없으면 입력을 무시
			!BoneGizmo.GetTarget()
			)
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

	// 클릭한 프레임에만 시작하고, 버튼을 놓는 프레임의 마지막 마우스 위치도 반영합니다.
	const bool bUpdateDrag = bWasUsing || (PointerInput.bMousePressed && PointerInput.bMouseDown);
	BoneGizmo.Update(MouseRay, MousePosition, ViewportClient->GetViewProjectionMatrix(),
		Width, Height, bUpdateDrag, ViewportClient->GetViewLocation(), !ViewportClient->IsPerspective());

	if (!bWasUsing && BoneGizmo.IsUsing())
	{
		const int32 ParentIndex = Mesh->GetRefSkeleton()[SelectedBoneIndex].ParentIndex;
		const FMatrix ParentWorld = ParentIndex >= 0
			? MeshComponent->GetBoneWorldMatrix(ParentIndex) : MeshComponent->GetWorldMatrix();

		// 스케일이 0인 부모의 역행렬로 이동을 계산할 수 없습니다.
		if (BoneGizmo.GetMode() == EGizmoMode::Location && fabsf(ParentWorld.Determinant()) < 1e-12f)
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

void FSkeletalMeshEditor::ApplyBoneGizmoTransform()
{
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
	}

	// 조작한 성분만 변경하고 나머지는 드래그 시작 시점의 로컬 값을 유지합니다.
	MeshComponent->SetBoneLocalTransform(SelectedBoneIndex, Local);
	MeshComponent->RefreshBoneTransformsNow();
	bPoseUndoPending = true;
}

bool FSkeletalMeshEditor::GetSelectedBoneLocalTransform(FTransform& OutTransform) const
{
	if (!MeshComponent || SelectedBoneIndex < 0 || SelectedBoneIndex >= BoneItems.Num())
	{
		return false;
	}

	OutTransform = MeshComponent->GetBoneLocalTransform(SelectedBoneIndex);
	return true;
}

void FSkeletalMeshEditor::SetSelectedBoneLocalTransform(const FTransform& Transform)
{
	if (!MeshComponent || SelectedBoneIndex < 0 || SelectedBoneIndex >= BoneItems.Num())
	{
		return;
	}

	// 원본 애셋 대신 프리뷰 컴포넌트의 포즈만 수정합니다. 본 갱신은 다음 World Tick에서 수행합니다.
	BoneGizmo.EndDrag();
	MeshComponent->SetBoneLocalTransform(SelectedBoneIndex, Transform);
	bPoseUndoPending = true;
}

void FSkeletalMeshEditor::ResetPose()
{
	if (!MeshComponent)
	{
		return;
	}

	BoneGizmo.EndDrag();
	MeshComponent->ResetPose();
	bPoseUndoPending = true;
}

void FSkeletalMeshEditor::ResetBone(int32 BoneIndex)
{
	if (!Mesh)
	{
		return;
	}

	if (!MeshComponent)
	{
		return;
	}

	if (BoneIndex < 0 || BoneIndex >= Mesh->GetRefSkeleton().Num())
	{
		return;
	}

	BoneGizmo.EndDrag();
	FTransform Transform = Mesh->GetRefSkeleton()[BoneIndex].LocalBindPose;
	MeshComponent->SetBoneLocalTransform(BoneIndex, Transform);
	bPoseUndoPending = true;
}

namespace
{
	bool AreBonePosesEqual(const TArray<FTransform>& A, const TArray<FTransform>& B)
	{
		if (A.Num() != B.Num())
		{
			return false;
		}
		for (int32 i = 0; i < A.Num(); ++i)
		{
			if (A[i].Location.X != B[i].Location.X || A[i].Location.Y != B[i].Location.Y || A[i].Location.Z != B[i].Location.Z ||
				A[i].Rotation.X != B[i].Rotation.X || A[i].Rotation.Y != B[i].Rotation.Y || A[i].Rotation.Z != B[i].Rotation.Z || A[i].Rotation.W != B[i].Rotation.W ||
				A[i].Scale.X != B[i].Scale.X || A[i].Scale.Y != B[i].Scale.Y || A[i].Scale.Z != B[i].Scale.Z)
			{
				return false;
			}
		}
		return true;
	}
}

TArray<FTransform> FSkeletalMeshEditor::CaptureBonePose() const
{
	TArray<FTransform> Pose;
	if (!Mesh || !MeshComponent)
	{
		return Pose;
	}

	const int32 NumBones = Mesh->GetRefSkeleton().Num();
	Pose.Reserve(NumBones);
	for (int32 i = 0; i < NumBones; ++i)
	{
		Pose.Add(MeshComponent->GetBoneLocalTransform(i));
	}
	return Pose;
}

void FSkeletalMeshEditor::ApplyBonePose(const TArray<FTransform>& Pose)
{
	if (!Mesh || !MeshComponent)
	{
		return;
	}

	BoneGizmo.EndDrag();
	const int32 NumBones = std::min(static_cast<int32>(Pose.Num()), static_cast<int32>(Mesh->GetRefSkeleton().Num()));
	for (int32 i = 0; i < NumBones; ++i)
	{
		MeshComponent->SetBoneLocalTransform(i, Pose[i]);
	}
	MeshComponent->RefreshBoneTransformsNow();
	SyncBoneGizmo();
}

void FSkeletalMeshEditor::ResetBonePoseHistory()
{
	PoseUndoStack.Empty();
	PoseRedoStack.Empty();
	CommittedBonePose = CaptureBonePose();
	bPoseUndoPending = false;
}

void FSkeletalMeshEditor::CommitBonePoseUndo(bool bInteractionActive)
{
	if (!bPoseUndoPending || bInteractionActive || !MeshComponent)
	{
		return;
	}
	bPoseUndoPending = false;

	TArray<FTransform> Current = CaptureBonePose();
	if (AreBonePosesEqual(Current, CommittedBonePose))
	{
		return;
	}

	PoseUndoStack.Add(std::move(CommittedBonePose));
	if (PoseUndoStack.Num() > MaxUndoSteps)
	{
		PoseUndoStack.RemoveAt(0);
	}
	PoseRedoStack.Empty();
	CommittedBonePose = std::move(Current);
}

void FSkeletalMeshEditor::UndoPoseEdit()
{
	CommitBonePoseUndo(false);
	if (PoseUndoStack.IsEmpty() || !MeshComponent)
	{
		return;
	}

	PoseRedoStack.Add(std::move(CommittedBonePose));
	CommittedBonePose = std::move(PoseUndoStack[PoseUndoStack.Num() - 1]);
	PoseUndoStack.RemoveAt(PoseUndoStack.Num() - 1);
	ApplyBonePose(CommittedBonePose);
}

void FSkeletalMeshEditor::RedoPoseEdit()
{
	CommitBonePoseUndo(false);
	if (PoseRedoStack.IsEmpty() || !MeshComponent)
	{
		return;
	}

	PoseUndoStack.Add(std::move(CommittedBonePose));
	CommittedBonePose = std::move(PoseRedoStack[PoseRedoStack.Num() - 1]);
	PoseRedoStack.RemoveAt(PoseRedoStack.Num() - 1);
	ApplyBonePose(CommittedBonePose);
}

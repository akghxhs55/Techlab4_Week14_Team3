#include "EnginePCH.h"
#include "SkeletalMeshDetailsPanel.h"
#include "Editor/SkeletalMeshEditor/SkeletalMeshEditor.h"
#include "Render/SkeletalMesh.h"
#include "Render/StockingRegion.h"
#include "Asset/AssetUtils.h"
#include "Math/Transform.h"
#include "Editor/Details/PropertyEditor.h"
#include <cstdio>

FSkeletalMeshDetailsPanel::FSkeletalMeshDetailsPanel(FSkeletalMeshEditor& InEditor)
	: FSkeletalMeshEditorPanel(InEditor, "Details")
{
}

void FSkeletalMeshDetailsPanel::DrawContent()
{
	const int32 BoneIndex = Editor.GetSelectedBoneIndex();

	if (ImGui::Button("Reset Transform"))
	{
		Editor.ResetBone(BoneIndex);
	}
	ImGui::SameLine();
	if (ImGui::Button("Reset All Transform"))
	{
		Editor.ResetPose();
	}

	ImGui::Separator();

	FTransform Transform;
	if (!Editor.GetSelectedBoneLocalTransform(Transform))
	{
		ImGui::TextDisabled("Select a bone in Bone Hierarchy.");
	}
	else
	{
		// 선택이 바뀌면 이전 본의 편집 상태가 새 본에 이어지지 않도록 ID를 구분합니다.
		ImGui::PushID(BoneIndex);
		bool bChanged = false;
		// 레벨 디테일 패널과 같은 X/Y/Z 축 컨트롤을 쓴다 (축 버튼을 누르면 그 축만 초기화).
		bChanged |= FPropertyEditor::DrawVector3("Location", Transform.Location.V, 0.0f);
		// 드래그 중 값이 튀지 않도록 마지막 오일러 값을 기억한다
		static FRotationConversionCache RotationCache;
		FRotator Euler = RotationCache.QuatToRotator(Transform.Rotation);
		if (FPropertyEditor::DrawRotator("Rotation", Euler))
		{
			Transform.Rotation = RotationCache.RotatorToQuat(Euler);
			bChanged = true;
		}
		bChanged |= FPropertyEditor::DrawVector3("Scale", Transform.Scale.V, 1.0f);
		ImGui::PopID();

		if (bChanged)
		{
			Editor.SetSelectedBoneLocalTransform(Transform);
		}
	}

	USkeletalMesh* Mesh = Editor.GetMesh();
	if (!Mesh)
	{
		return;
	}

	ImGui::Separator();
	if (ImGui::CollapsingHeader("Sockets", ImGuiTreeNodeFlags_DefaultOpen))
	{
		static char NewSocketName[128] = "WeaponSocket";
		ImGui::InputText("Socket Name", NewSocketName, sizeof(NewSocketName));
		ImGui::SameLine();
		if (ImGui::Button("Add Socket"))
		{
			const TArray<FBoneInfo>& Bones = Mesh->GetRefSkeleton();
			const int32 TargetBoneIndex = (BoneIndex >= 0 && BoneIndex < Bones.Num()) ? BoneIndex : (Bones.IsEmpty() ? -1 : 0);
			if (TargetBoneIndex >= 0)
			{
				FSkeletalMeshSocket NewSocket;
				NewSocket.SocketName = NewSocketName;
				NewSocket.BoneName = Bones[TargetBoneIndex].Name;
				Mesh->AddSocket(NewSocket);
			}
		}

		ImGui::SameLine();
		if (ImGui::Button("Save Sockets"))
		{
			const FString FilePath = FAssetUtils::ResolveAssetFilePath(Editor.GetAssetPath());
			if (!FilePath.empty())
			{
				Mesh->SaveSocketsToFile(FilePath + ".sockets.json");
			}
		}

		static FString SelectedSocketName;
		const TArray<FSkeletalMeshSocket>& Sockets = Mesh->GetSockets();
		for (int32 Idx = 0; Idx < Sockets.Num(); ++Idx)
		{
			ImGui::PushID(Idx);
			const FSkeletalMeshSocket& Socket = Sockets[Idx];
			const bool bSelected = (SelectedSocketName == Socket.SocketName);
			const FString Label = Socket.SocketName + " (" + Socket.BoneName + ")";
			if (ImGui::Selectable(Label.c_str(), bSelected))
			{
				SelectedSocketName = Socket.SocketName;
			}
			ImGui::PopID();
		}

		if (FSkeletalMeshSocket* MutableSocket = Mesh->FindMutableSocket(SelectedSocketName))
		{
			ImGui::Separator();
			ImGui::Text("Selected Socket: %s", MutableSocket->SocketName.c_str());
			ImGui::Text("Attached Bone: %s", MutableSocket->BoneName.c_str());
			if (ImGui::BeginCombo("Bone", MutableSocket->BoneName.c_str()))
			{
				for (const FBoneInfo& Bone : Mesh->GetRefSkeleton())
				{
					const bool bSelected = (Bone.Name == MutableSocket->BoneName);
					if (ImGui::Selectable(Bone.Name.c_str(), bSelected))
					{
						MutableSocket->BoneName = Bone.Name;
					}
				}
				ImGui::EndCombo();
			}
			FPropertyEditor::DrawVector3("Relative Location", MutableSocket->RelativeLocation.V, 0.0f);
			float RotAngles[3] = { MutableSocket->RelativeRotation.Pitch, MutableSocket->RelativeRotation.Yaw, MutableSocket->RelativeRotation.Roll };
			if (ImGui::DragFloat3("Relative Rotation", RotAngles, 0.5f))
			{
				MutableSocket->RelativeRotation = FRotator(RotAngles[0], RotAngles[1], RotAngles[2]);
			}
			FPropertyEditor::DrawVector3("Relative Scale", MutableSocket->RelativeScale.V, 1.0f);
			if (ImGui::Button("Remove Selected Socket"))
			{
				const FString ToRemove = SelectedSocketName;
				SelectedSocketName.clear();
				Mesh->RemoveSocket(ToRemove);
			}
		}
	}

	ImGui::Separator();
	if (ImGui::CollapsingHeader("Stocking Region", ImGuiTreeNodeFlags_DefaultOpen))
	{
		if (UStockingRegion* Region = Editor.GetEditingStockingRegion())
		{
			bool bSelectMode = Editor.IsRegionSelectMode();
			if (ImGui::Checkbox("Triangle Select Mode", &bSelectMode))
			{
				Editor.SetRegionSelectMode(bSelectMode);
			}

			bool bEraseMode = Editor.IsRegionEraseMode();
			ImGui::SameLine();
			if (ImGui::Checkbox("Erase", &bEraseMode))
			{
				Editor.SetRegionEraseMode(bEraseMode);
			}

			float BrushRadius = Editor.GetRegionBrushRadius();
			if (ImGui::SliderFloat("Brush Radius", &BrushRadius, 0.0f, 0.4f, "%.3f"))
			{
				Editor.SetRegionBrushRadius(BrushRadius);
			}

			ImGui::SliderInt("Subdivision", &Region->SubdivisionIterations, 0, 2);
			ImGui::DragFloat("Surface Offset", &Region->SurfaceOffset, 0.0005f, 0.0f, 0.05f, "%.4f");
			ImGui::DragFloat("Boundary Feather", &Region->BoundaryFeatherDistance, 0.002f, 0.005f, 0.3f, "%.3f");

			if (ImGui::Button("Select Bone Tris"))
			{
				Editor.SelectRegionBySelectedBone();
			}
			ImGui::SameLine();
			if (ImGui::Button("Grow"))
			{
				Editor.GrowStockingRegion();
			}
			ImGui::SameLine();
			if (ImGui::Button("Shrink"))
			{
				Editor.ShrinkStockingRegion();
			}
			ImGui::SameLine();
			if (ImGui::Button("Clear"))
			{
				Editor.ClearStockingRegion();
			}

			if (ImGui::Button("Save Region"))
			{
				FString Message;
				Editor.SaveStockingRegion(Message);
			}
			ImGui::SameLine();
			if (ImGui::Button("Load Region"))
			{
				FString Message;
				Editor.LoadStockingRegion(Message);
			}
			ImGui::SameLine();
			if (ImGui::Button("Validate"))
			{
				FString Message;
				Editor.ValidateStockingRegion(Message);
			}

			ImGui::Text("Selected Triangles: %d", Region->GetSelectedTriangleCount());
			const ImVec4 StatusColor = Editor.IsStockingRegionValid()
				? ImVec4(0.35f, 0.90f, 0.45f, 1.0f)
				: ImVec4(0.95f, 0.65f, 0.25f, 1.0f);
			ImGui::TextColored(StatusColor, "%s", Editor.GetStockingRegionStatus().c_str());

			if (ImGui::Button("Open in Clothing Editor"))
			{
				Editor.OpenInClothingEditor();
			}
		}
	}
}

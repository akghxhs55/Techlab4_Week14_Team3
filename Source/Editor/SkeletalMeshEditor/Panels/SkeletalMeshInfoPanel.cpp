#include "EnginePCH.h"
#include "SkeletalMeshInfoPanel.h"
#include "Editor/SkeletalMeshEditor/SkeletalMeshEditor.h"
#include "Editor/SkeletalMeshEditor/PhysicsAssetUtils.h"
#include "Render/SkeletalMesh.h"

#include <filesystem>

FSkeletalMeshInfoPanel::FSkeletalMeshInfoPanel(FSkeletalMeshEditor& InEditor)
	: FSkeletalMeshEditorPanel(InEditor, "Info")
{
}

void FSkeletalMeshInfoPanel::DrawContent()
{
	USkeletalMesh* Mesh = Editor.GetMesh();
	if (!Mesh)
	{
		return;
	}

	const FString Name = std::filesystem::path(Editor.GetAssetPath()).stem().string();
	ImGui::TextDisabled("Asset Name");
	ImGui::TextWrapped("%s", Name.c_str());
	ImGui::Spacing();
	ImGui::TextDisabled("Asset Path");
	ImGui::TextWrapped("%s", Editor.GetAssetPath().c_str());
	ImGui::Separator();

	// 현재 화면에서 선택된 LOD가 아닌 원본 LOD0의 통계를 표시합니다.
	const FSkeletalMeshLODRenderData& Data = Mesh->GetRenderData().LODRenderData[0];
	ImGui::Text("Vertices: %d", static_cast<int>(Data.BindVertices.Num()));
	ImGui::Text("Triangles: %d", static_cast<int>(Data.Indices.Num() / 3));
	ImGui::Text("Materials: %d", static_cast<int>(Mesh->GetMaterialSlots().Num()));
	ImGui::Text("Sections: %d", static_cast<int>(Data.Sections.Num()));
	ImGui::Text("Bones: %d", Mesh->GetNumBones());
	ImGui::Separator();
	ImGui::Text("Physics Asset: %s", Mesh->GetPhysicsAsset() ? "Loaded" : "None");
	ImGui::SliderFloat("Min Bone Size", &MinBoneSize, 0.01f, 0.5f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	ImGui::SliderInt("Min Vertices", &MinVertexCount, 1, 100, "%d", ImGuiSliderFlags_AlwaysClamp);
	ImGui::Combo("Primitive Type", &PrimitiveType, "Capsule\0Box\0");
	ImGui::Checkbox("Auto Orient to Bone", &bAutoOrientToBone);
	ImGui::Checkbox("Include Child Bones", &bIncludeChildBones);
	ImGui::Combo("Angular Constraint", &AngularMode, "Limited\0Locked\0Free\0");
	if (ImGui::Button("Create Physics Asset"))
	{
		FPhysAssetCreateParams Params;
		Params.MinBoneSize = MinBoneSize;
		Params.MinVertexCount = MinVertexCount;
		Params.GeomType = PrimitiveType == 0 ? EPhysAssetFitGeomType::Capsule : EPhysAssetFitGeomType::Box;
		Params.AngularMode = static_cast<EPhysAssetAngularMode>(AngularMode);
		Params.bAutoOrientToBone = bAutoOrientToBone;
		Params.bIncludeChildBones = bIncludeChildBones;
		if (Editor.CreatePhysicsAsset(Params, PhysicsAssetMessage))
			if (PhysicsAssetMessage.empty()) PhysicsAssetMessage = "Physics Asset created";
	}
	if (!PhysicsAssetMessage.empty())
		ImGui::TextWrapped("%s", PhysicsAssetMessage.c_str());
}

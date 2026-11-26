#include "EnginePCH.h"
#include "StaticMeshDetailsPanel.h"
#include "Editor/StaticMeshEditor/StaticMeshEditor.h"
#include "Asset/LOD/StaticMeshLODGenerator.h"
#include "Asset/AssetManager.h"
#include "Render/Mesh.h"

FStaticMeshDetailsPanel::FStaticMeshDetailsPanel(FStaticMeshEditor& InEditor)
	: FStaticMeshEditorPanel(InEditor, "Details")
{
}

void FStaticMeshDetailsPanel::DrawContent()
{
	UStaticMesh* Mesh = Editor.GetMesh();
	if (!Mesh)
	{
		return;
	}

	if (ImGui::CollapsingHeader("LOD Settings", ImGuiTreeNodeFlags_DefaultOpen))
	{
		ImGui::Text("LOD Count: %u", Mesh->GetLODCount());
		for (uint32 LOD = 0; LOD < Mesh->GetLODCount(); ++LOD)
		{
			const FStaticMeshData& Data = Mesh->GetLODMeshData(LOD);
			ImGui::Text("LOD%u: %d tris, %d verts", LOD, Data.Indices.Num() / 3, Data.Vertices.Num());
		}

		ImGui::Spacing();
		ImGui::DragFloat3("Screen Thresholds", Mesh->ScreenThresholds.data(), 0.005f, 0.001f, 1.0f, "%.3f");
		if (ImGui::Button("Generate LODs"))
		{
			FLODGenerateRequest Request;
			Request.ScreenThresholds = Mesh->ScreenThresholds;
			const FLODGenerateResult Result = UAssetManager::GenerateStaticMeshLODs(*Mesh, Request);
			if (!Result.bSuccess)
			{
				KRS_LOG(Warning, "GenerateStaticMeshLODs failed: {}", Result.FailureReason);
			}
		}
	}
}

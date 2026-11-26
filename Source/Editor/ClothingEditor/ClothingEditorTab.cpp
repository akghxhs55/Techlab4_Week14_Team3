#include "EnginePCH.h"
#include "Editor/ClothingEditor/ClothingEditorTab.h"
#include "Editor/ClothingEditor/ClothingEditor.h"
#include "Editor/ClothingEditor/Panels/ClothingViewportPanel.h"
#include "Editor/ClothingEditor/Panels/ClothingOutlinerPanel.h"
#include "Editor/ClothingEditor/Panels/ClothingInspectorPanel.h"
#include "Components/SkinnedMeshComponent.h"

FClothingEditorTab::FClothingEditorTab(FClothingEditor& InEditor)
	: FAssetEditorTab(InEditor)
	, ClothingEditor(InEditor)
{
	const ImGuiID AssetId = ImHashStr(InEditor.GetAssetPath().c_str());
	DockSpaceId = ImHashStr("ClothingEditorDockSpace", 0, AssetId);
	PanelClassId = ImHashStr("ClothingEditorPanelClass", 0, AssetId);

	Panels.Add(MakeUnique<FClothingViewportPanel>(InEditor));
	Panels.Add(MakeUnique<FClothingOutlinerPanel>(InEditor));
	Panels.Add(MakeUnique<FClothingInspectorPanel>(InEditor));

	for (int32 i = 0; i < Panels.Num(); ++i)
	{
		Panels[i]->SetDockClass(PanelClassId);
		Panels[i]->Init();
	}
}

void FClothingEditorTab::DrawMenuBar()
{
	if (ImGui::BeginMenu("File"))
	{
		if (ImGui::MenuItem("Save Selected Stocking Region"))
		{
			FString Message;
			ClothingEditor.SaveStockingLayerRegion(ClothingEditor.GetSelectedLayerIndex(), Message);
		}
		if (ImGui::MenuItem("Close Editor"))
		{
			ClothingEditor.Close();
		}
		ImGui::EndMenu();
	}

	if (ImGui::BeginMenu("Clothing"))
	{
		if (ImGui::MenuItem("Add Stocking (Auto Legs)"))
		{
			ClothingEditor.AddStockingLayer("Stocking", true);
		}
		if (ImGui::MenuItem("Add Stocking (Empty Region)"))
		{
			ClothingEditor.AddStockingLayer("Custom Stocking", false);
		}
		ImGui::Separator();
		if (ImGui::MenuItem("Add Cape (NvCloth Grid)"))
		{
			ClothingEditor.AddCapeClothLayer("Cape");
		}
		if (ImGui::MenuItem("Add Skirt (NvCloth Cone)"))
		{
			ClothingEditor.AddSkirtClothLayer("Skirt");
		}
		if (ImGui::MenuItem("Add StaticMesh Cloth"))
		{
			ClothingEditor.AddStaticMeshClothLayer("StaticMesh Cloth");
		}
		ImGui::Separator();
		if (ImGui::MenuItem("Reset Cloth & Pull Poses"))
		{
			ClothingEditor.ResetAllClothPoses();
		}
		if (ImGui::MenuItem("Reset Character Pose"))
		{
			ClothingEditor.ResetCharacterPose();
		}
		ImGui::EndMenu();
	}

	if (ImGui::BeginMenu("Skinning"))
	{
		const bool bGpuSkin = USkinnedMeshComponent::bUseGPUSkinning;
		if (ImGui::MenuItem("CPU Skinning", nullptr, !bGpuSkin))
		{
			USkinnedMeshComponent::SetGPUSkinningEnabled(false);
		}
		if (ImGui::MenuItem("GPU Skinning", nullptr, bGpuSkin))
		{
			USkinnedMeshComponent::SetGPUSkinningEnabled(true);
		}
		ImGui::EndMenu();
	}

	EditorDock::DrawWindowMenu(Panels);
}

void FClothingEditorTab::BuildDefaultLayout(ImGuiID InDockSpaceId, const ImVec2& Size)
{
	ImGui::DockBuilderAddNode(InDockSpaceId, ImGuiDockNodeFlags_DockSpace);
	ImGui::DockBuilderSetNodeSize(InDockSpaceId, Size);

	ImGuiID ViewportId = InDockSpaceId;
	const ImGuiID LeftId = ImGui::DockBuilderSplitNode(ViewportId, ImGuiDir_Left, 0.22f, nullptr, &ViewportId);
	const ImGuiID RightId = ImGui::DockBuilderSplitNode(ViewportId, ImGuiDir_Right, 0.28f, nullptr, &ViewportId);

	ImGui::DockBuilderDockWindow(Panels[0]->GetWindowName(), ViewportId);
	ImGui::DockBuilderDockWindow(Panels[1]->GetWindowName(), LeftId);
	ImGui::DockBuilderDockWindow(Panels[2]->GetWindowName(), RightId);
	ImGui::DockBuilderFinish(InDockSpaceId);
}

void FClothingEditorTab::OnRender(bool bActive)
{
	ClothingEditor.SetViewportState(0, 0, {});
	if (ClothingEditor.IsCloseRequested())
	{
		return;
	}

	if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_DockingEnable)
	{
		const ImVec2 Size = ImGui::GetContentRegionAvail();
		if (bActive && Size.x > 0.0f && Size.y > 0.0f &&
			(!bLayoutInitialized || !ImGui::DockBuilderGetNode(DockSpaceId)))
		{
			ImGui::DockBuilderRemoveNode(DockSpaceId);
			BuildDefaultLayout(DockSpaceId, Size);
			bLayoutInitialized = true;
		}

		const ImGuiWindowClass WindowClass = EditorDock::MakeDockSpaceClass(PanelClassId);
		const ImGuiDockNodeFlags Flags = bActive ? ImGuiDockNodeFlags_None : ImGuiDockNodeFlags_KeepAliveOnly;
		if (bActive || ImGui::DockBuilderGetNode(DockSpaceId))
		{
			ImGui::DockSpace(DockSpaceId, ImVec2(0.0f, 0.0f), Flags, &WindowClass);
		}
	}

	for (int32 i = 0; i < Panels.Num(); ++i)
	{
		if (Panels[i]->IsOpen() && (bActive || Panels[i]->IsDetached(DockSpaceId)))
		{
			Panels[i]->OnRender();
		}
	}
}

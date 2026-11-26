#include "EnginePCH.h"
#include "PhysicsAssetEditorTab.h"
#include "PhysicsAssetEditor.h"
#include "Panels/PhysicsAssetViewportPanel.h"
#include "Panels/PhysicsAssetHierarchyPanel.h"
#include "Panels/PhysicsAssetDetailsPanel.h"
#include "Panels/PhysicsAssetSettingsPanel.h"
#include "Panels/PhysicsAssetGraphPanel.h"

FPhysicsAssetEditorTab::FPhysicsAssetEditorTab(FPhysicsAssetEditor& InEditor)
	: FAssetEditorTab(InEditor)
	, PhysicsEditor(InEditor)
{
	const ImGuiID AssetId = ImHashStr(InEditor.GetAssetPath().c_str());
	DockSpaceId = ImHashStr("PhysicsAssetDockSpace", 0, AssetId);
	PanelClassId = ImHashStr("PhysicsAssetPanelClass", 0, AssetId);

	Panels.Add(MakeUnique<FPhysicsAssetViewportPanel>(InEditor));
	Panels.Add(MakeUnique<FPhysicsAssetHierarchyPanel>(InEditor));
	Panels.Add(MakeUnique<FPhysicsAssetDetailsPanel>(InEditor));
	Panels.Add(MakeUnique<FPhysicsAssetSettingsPanel>(InEditor));
	Panels.Add(MakeUnique<FPhysicsAssetGraphPanel>(InEditor));
	for (auto& Panel : Panels)
	{
		Panel->SetDockClass(PanelClassId);
		Panel->Init();
	}
}

void FPhysicsAssetEditorTab::DrawMenuBar()
{
	if (ImGui::BeginMenu("File"))
	{
		if (ImGui::MenuItem("Save", "Ctrl+S", false, PhysicsEditor.GetPhysicsAsset() != nullptr)) PhysicsEditor.Save();
		ImGui::Separator();
		if (ImGui::MenuItem("Close Editor")) PhysicsEditor.Close();
		ImGui::EndMenu();
	}
	if (ImGui::BeginMenu("Simulation"))
	{
		const bool bSimulating = PhysicsEditor.IsSimulationActive();
		if (ImGui::MenuItem("Simulate", nullptr, bSimulating, bSimulating || PhysicsEditor.CanStartSimulation()))
			PhysicsEditor.ToggleSimulation();
		ImGui::EndMenu();
	}
	EditorDock::DrawWindowMenu(Panels);
}

void FPhysicsAssetEditorTab::BuildDefaultLayout(ImGuiID InDockSpaceId, const ImVec2& Size)
{
	ImGui::DockBuilderAddNode(InDockSpaceId, ImGuiDockNodeFlags_DockSpace);
	ImGui::DockBuilderSetNodeSize(InDockSpaceId, Size);

	ImGuiID ViewportId = InDockSpaceId;
	const ImGuiID HierarchyId = ImGui::DockBuilderSplitNode(ViewportId, ImGuiDir_Left, 0.22f, nullptr, &ViewportId);
	ImGuiID DetailsId = ImGui::DockBuilderSplitNode(ViewportId, ImGuiDir_Right, 0.28f, nullptr, &ViewportId);
	const ImGuiID SettingsId = ImGui::DockBuilderSplitNode(DetailsId, ImGuiDir_Down, 0.40f, nullptr, &DetailsId);
	const ImGuiID GraphId = ImGui::DockBuilderSplitNode(ViewportId, ImGuiDir_Down, 0.30f, nullptr, &ViewportId);
	ImGui::DockBuilderDockWindow(Panels[0]->GetWindowName(), ViewportId);
	ImGui::DockBuilderDockWindow(Panels[1]->GetWindowName(), HierarchyId);
	ImGui::DockBuilderDockWindow(Panels[2]->GetWindowName(), DetailsId);
	ImGui::DockBuilderDockWindow(Panels[3]->GetWindowName(), SettingsId);
	ImGui::DockBuilderDockWindow(Panels[4]->GetWindowName(), GraphId);
	ImGui::DockBuilderFinish(InDockSpaceId);
}

void FPhysicsAssetEditorTab::OnRender(bool bActive)
{
	// 뷰포트 패널이 렌더링될 때 해당 패널이 크기와 입력을 다시 설정합니다.
	PhysicsEditor.SetViewportState(0, 0, {});
	if (PhysicsEditor.IsCloseRequested())
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

	for (auto& Panel : Panels)
	{
		// 분리된 패널은 부모 탭이 가려져도 표시합니다.
		if (Panel->IsOpen() && (bActive || Panel->IsDetached(DockSpaceId)))
		{
			Panel->OnRender();
		}
	}
	const ImGuiIO& IO = ImGui::GetIO();
	const bool bTabFocused = bActive && ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows | ImGuiFocusedFlags_DockHierarchy);
	bool bPanelFocused = false;
	for (const auto& Panel : Panels)
	{
		// 비활성 탭의 도킹 패널은 이번 프레임에 렌더링되지 않아 이전 포커스가 남을 수 있습니다.
		if (Panel->IsOpen() && Panel->IsFocused() && (bActive || Panel->IsDetached(DockSpaceId)))
		{
			bPanelFocused = true;
			break;
		}
	}
	if ((bTabFocused || bPanelFocused) && IO.KeyCtrl && !IO.KeyAlt && !IO.KeyShift &&
		!IO.WantTextInput && !IO.AppFocusLost && ImGui::IsKeyPressed(ImGuiKey_S, false)) PhysicsEditor.Save();
}

#include "EnginePCH.h"
#include "AimOffsetEditorTab.h"
#include "AimOffsetEditor.h"
#include "AimOffsetPanel.h"
#include "Editor/SkeletalPreviewEditor/SkeletalPreviewViewportPanel.h"

FAimOffsetEditorTab::FAimOffsetEditorTab(FAimOffsetEditor& InEditor)
	: FDockedAssetEditorTab(InEditor, "AimOffsetDockSpace", "AimOffsetPanelClass")
	, AimEditor(InEditor)
{
	RegisterPanel(MakeUnique<FSkeletalPreviewViewportPanel>(InEditor));
	RegisterPanel(MakeUnique<FAimOffsetPanel>(InEditor));
}

void FAimOffsetEditorTab::BeforeRender()
{
	AimEditor.SetViewportState(0, 0, {});
}

void FAimOffsetEditorTab::BuildDefaultLayout(ImGuiID InDockSpaceId, const ImVec2& Size)
{
	ImGui::DockBuilderAddNode(InDockSpaceId, ImGuiDockNodeFlags_DockSpace);
	ImGui::DockBuilderSetNodeSize(InDockSpaceId, Size);

	ImGuiID ViewportId = InDockSpaceId;
	const ImGuiID ControlsId = ImGui::DockBuilderSplitNode(ViewportId, ImGuiDir_Right, 0.34f, nullptr, &ViewportId);

	ImGui::DockBuilderDockWindow(Panels[0]->GetWindowName(), ViewportId);
	ImGui::DockBuilderDockWindow(Panels[1]->GetWindowName(), ControlsId);
	ImGui::DockBuilderFinish(InDockSpaceId);
}

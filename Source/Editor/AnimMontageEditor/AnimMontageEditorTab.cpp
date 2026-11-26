#include "EnginePCH.h"
#include "AnimMontageEditorTab.h"
#include "AnimMontageEditor.h"
#include "AnimMontagePanel.h"
#include "Editor/SkeletalPreviewEditor/SkeletalPreviewViewportPanel.h"

FAnimMontageEditorTab::FAnimMontageEditorTab(FAnimMontageEditor& InEditor)
	: FDockedAssetEditorTab(InEditor, "AnimMontageDockSpace", "AnimMontagePanelClass")
	, MontageEditor(InEditor)
{
	RegisterPanel(MakeUnique<FSkeletalPreviewViewportPanel>(InEditor));
	RegisterPanel(MakeUnique<FAnimMontageTimelinePanel>(InEditor));
	RegisterPanel(MakeUnique<FAnimMontagePanel>(InEditor));
}

void FAnimMontageEditorTab::BeforeRender()
{
	MontageEditor.SetViewportState(0, 0, {});
}

void FAnimMontageEditorTab::BuildDefaultLayout(ImGuiID InDockSpaceId, const ImVec2& Size)
{
	ImGui::DockBuilderAddNode(InDockSpaceId, ImGuiDockNodeFlags_DockSpace);
	ImGui::DockBuilderSetNodeSize(InDockSpaceId, Size);

	ImGuiID ViewportId = InDockSpaceId;
	const ImGuiID ControlsId = ImGui::DockBuilderSplitNode(ViewportId, ImGuiDir_Right, 0.30f, nullptr, &ViewportId);
	const ImGuiID TimelineId = ImGui::DockBuilderSplitNode(ViewportId, ImGuiDir_Down, 0.30f, nullptr, &ViewportId);

	ImGui::DockBuilderDockWindow(Panels[0]->GetWindowName(), ViewportId);
	ImGui::DockBuilderDockWindow(Panels[1]->GetWindowName(), TimelineId);
	ImGui::DockBuilderDockWindow(Panels[2]->GetWindowName(), ControlsId);
	ImGui::DockBuilderFinish(InDockSpaceId);
}

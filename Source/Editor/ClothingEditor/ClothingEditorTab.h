#pragma once

#include "Editor/AssetEditor/AssetEditorTab.h"
#include "Editor/AssetEditor/AssetEditorPanel.h"

class FClothingEditor;

class FClothingEditorPanel : public TAssetEditorPanel<FClothingEditor>
{
public:
	FClothingEditorPanel(FClothingEditor& InEditor, const char* InName)
		: TAssetEditorPanel<FClothingEditor>(InEditor, InName, "ClothingPanel")
	{
	}
};

class FClothingEditorTab : public FAssetEditorTab
{
public:
	explicit FClothingEditorTab(FClothingEditor& InEditor);

	void DrawMenuBar() override;
	bool HasFocusedSubWindow() const override { return EditorDock::HasFocusedPanel(Panels); }
	void OnRender(bool bActive) override;

private:
	void BuildDefaultLayout(ImGuiID InDockSpaceId, const ImVec2& Size);

	FClothingEditor& ClothingEditor;
	TArray<TUniquePtr<FClothingEditorPanel>> Panels;
	ImGuiID DockSpaceId = 0;
	ImGuiID PanelClassId = 0;
	bool bLayoutInitialized = false;
};

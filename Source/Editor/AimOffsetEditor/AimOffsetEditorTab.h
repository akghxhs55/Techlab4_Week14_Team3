#pragma once

#include "Editor/AssetEditor/DockedAssetEditorTab.h"

class FAimOffsetEditor;

class FAimOffsetEditorTab : public FDockedAssetEditorTab
{
public:
	explicit FAimOffsetEditorTab(FAimOffsetEditor& InEditor);

private:
	void BeforeRender() override;
	void BuildDefaultLayout(ImGuiID InDockSpaceId, const ImVec2& Size) override;

	FAimOffsetEditor& AimEditor;
};

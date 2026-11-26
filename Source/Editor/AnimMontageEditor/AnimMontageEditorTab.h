#pragma once

#include "Editor/AssetEditor/DockedAssetEditorTab.h"

class FAnimMontageEditor;

class FAnimMontageEditorTab : public FDockedAssetEditorTab
{
public:
	explicit FAnimMontageEditorTab(FAnimMontageEditor& InEditor);

private:
	void BeforeRender() override;
	void BuildDefaultLayout(ImGuiID InDockSpaceId, const ImVec2& Size) override;

	FAnimMontageEditor& MontageEditor;
};

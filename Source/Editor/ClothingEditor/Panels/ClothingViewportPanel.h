#pragma once

#include "Editor/ClothingEditor/ClothingEditorTab.h"
#include "Editor/SkeletalPreviewEditor/SkeletalPreviewViewportPanel.h"

class FClothingViewportPanel : public FClothingEditorPanel
{
public:
	explicit FClothingViewportPanel(FClothingEditor& InEditor);

protected:
	void DrawContent() override;
	bool IsViewportPanel() const override { return true; }

private:
	void DrawClothingToolStrip();

	FPreviewViewportToolbarState ToolbarState;
};

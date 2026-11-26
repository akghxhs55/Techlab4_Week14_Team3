#pragma once

#include "Editor/ClothingEditor/ClothingEditorTab.h"
#include "Editor/AnimSequenceEditor/Panels/AnimBrowserPanels.h"

class FClothingOutlinerPanel : public FClothingEditorPanel
{
public:
	explicit FClothingOutlinerPanel(FClothingEditor& InEditor);

protected:
	void DrawContent() override;

private:
	void DrawLayersSection();
	void DrawAnimationSection();
	void DrawBoneTreeSection();
	void DrawBoneNodeRecursive(int32 BoneIndex);

	FAnimAssetBrowserView AnimBrowser;
	char BoneFilterBuffer[128] = {};
};

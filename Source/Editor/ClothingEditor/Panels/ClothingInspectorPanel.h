#pragma once

#include "Editor/ClothingEditor/ClothingEditorTab.h"

struct FClothingLayer;

class FClothingInspectorPanel : public FClothingEditorPanel
{
public:
	explicit FClothingInspectorPanel(FClothingEditor& InEditor);

protected:
	void DrawContent() override;

private:
	void DrawStockingInspector(int32 LayerIndex, FClothingLayer& Layer);
	void DrawClothInspector(int32 LayerIndex, FClothingLayer& Layer);
	void DrawBonePoseTestSection();
	void DrawDebugDisplaySection();
};

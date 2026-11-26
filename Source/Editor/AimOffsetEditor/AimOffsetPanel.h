#pragma once

#include "Editor/AssetEditor/AssetEditorPanel.h"
#include "AimOffsetEditor.h"
#include <vector>

class FAimOffsetPanel : public TAssetEditorPanel<FAimOffsetEditor>
{
public:
	explicit FAimOffsetPanel(FAimOffsetEditor& InEditor)
		: TAssetEditorPanel<FAimOffsetEditor>(InEditor, "AimOffset Settings", "AimOffsetPanelClass")
	{
	}

protected:
	void DrawContent() override;

private:
	void RefreshSequences();
	void DrawSequenceCombo(const char* Label, FString& InOutPath);

	std::vector<FString> SequencePaths;
	bool bInitialized = false;
};

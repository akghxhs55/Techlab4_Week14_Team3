#pragma once

#include "Editor/AssetEditor/AssetEditorPanel.h"

class FPhysicsAssetEditor;

// 피직스 에셋 에디터 전용 패널의 공통 베이스입니다.
class FPhysicsAssetEditorPanel : public TAssetEditorPanel<FPhysicsAssetEditor>
{
public:
	FPhysicsAssetEditorPanel(FPhysicsAssetEditor& InEditor, const char* InName);
};

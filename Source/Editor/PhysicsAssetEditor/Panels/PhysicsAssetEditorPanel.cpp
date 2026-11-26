#include "EnginePCH.h"
#include "PhysicsAssetEditorPanel.h"
#include "Editor/PhysicsAssetEditor/PhysicsAssetEditor.h"

// 패널 이름과 에셋 경로로 창 ID를 구분합니다.
FPhysicsAssetEditorPanel::FPhysicsAssetEditorPanel(FPhysicsAssetEditor& InEditor, const char* InName)
	: TAssetEditorPanel<FPhysicsAssetEditor>(InEditor, InName, "PhysicsAssetPanel")
{
}

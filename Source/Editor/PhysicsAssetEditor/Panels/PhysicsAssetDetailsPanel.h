#pragma once

#include "PhysicsAssetEditorPanel.h"

// 메시 상세 정보 UI를 확장하기 위한 패널 골격입니다.
class FPhysicsAssetDetailsPanel : public FPhysicsAssetEditorPanel
{
public:
	explicit FPhysicsAssetDetailsPanel(FPhysicsAssetEditor& InEditor);

protected:
	void DrawContent() override;
};

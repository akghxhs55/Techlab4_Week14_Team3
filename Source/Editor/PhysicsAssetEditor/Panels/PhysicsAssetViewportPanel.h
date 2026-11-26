#pragma once

#include "PhysicsAssetEditorPanel.h"
#include "Editor/SkeletalPreviewEditor/SkeletalPreviewViewportPanel.h"

// 프리뷰 렌더 결과를 표시하고 카메라 입력과 화면 크기를 Editor에 전달합니다.
class FPhysicsAssetViewportPanel : public FPhysicsAssetEditorPanel
{
public:
	explicit FPhysicsAssetViewportPanel(FPhysicsAssetEditor& InEditor);

protected:
	void DrawContent() override;
	bool IsViewportPanel() const override { return true; }

private:
	FPreviewViewportToolbarState ToolbarState;
};

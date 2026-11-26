#pragma once

#include "PhysicsAssetEditorPanel.h"

class FPhysicsAssetGraphPanel : public FPhysicsAssetEditorPanel
{
public:
	explicit FPhysicsAssetGraphPanel(FPhysicsAssetEditor& InEditor);

protected:
	void DrawContent() override;

private:
	ImVec2 Pan = ImVec2(20.0f, 20.0f);
	float Zoom = 1.0f;
	uint32 LastTopologyRevision = ~0u;
	int32 LastSelectedBody = -1;
	int32 LastSelectedConstraint = -1;
	bool bShowAllBodies = false;
	bool bFitRequested = true;
};

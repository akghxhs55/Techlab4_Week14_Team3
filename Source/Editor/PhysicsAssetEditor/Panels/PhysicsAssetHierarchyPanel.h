#pragma once

#include "PhysicsAssetEditorPanel.h"
#include "Containers/Array.h"

#include <functional>

// 피직스 에셋 에디터의 본 계층과 선택 상태를 표시합니다.
class FPhysicsAssetHierarchyPanel : public FPhysicsAssetEditorPanel
{
public:
	explicit FPhysicsAssetHierarchyPanel(FPhysicsAssetEditor& InEditor);

protected:
	void DrawContent() override;

private:
	struct FBodyTreeContext
	{
		const TArray<TArray<int32>>& Children;
		const TArray<uint8>& VisibleBones;
		const TArray<uint8>& SelectionAncestors;
	};
	void DrawBoneNode(int32 BoneIndex, const TArray<int32>& BodyByBone,
		const TArray<uint8>& VisibleBones, const TArray<uint8>& SelectionAncestors,
		bool bRevealSelection, bool& bScrollToSelection);
	void DrawBodyNode(int32 BodyIndex, bool bRevealSelection, bool& bScrollToSelection,
		const FBodyTreeContext* BodyTree = nullptr);
	bool bShowBones = true;
	bool bShowBodies = true;
	// 배열을 순회하는 동안에는 메뉴 요청만 보관하고 DrawContent 마지막에 적용합니다.
	std::function<void()> PendingEdit;
	char SearchBuffer[128] = {};
	int32 LastSelectedBone = -1;
	int32 LastSelectedBody = -1;
	int32 LastSelectedConstraint = -1;
	int32 LastSelectedShapeType = 0;
	int32 LastSelectedShapeIndex = -1;
	uint32 LastTopologyRevision = 0;
};

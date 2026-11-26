#pragma once

#include "SkeletalMeshEditorPanel.h"

// 원본 애셋의 식별 정보와 메시 통계를 읽기 전용으로 표시합니다. Physics Asset 생성 컨트롤도 제공합니다.
class FSkeletalMeshInfoPanel : public FSkeletalMeshEditorPanel
{
public:
	explicit FSkeletalMeshInfoPanel(FSkeletalMeshEditor& InEditor);

protected:
	void DrawContent() override;

private:
	FString PhysicsAssetMessage;
	float MinBoneSize = 0.08f;
	int32 MinVertexCount = 8;
	int32 PrimitiveType = 0;
	int32 AngularMode = 0;
	bool bAutoOrientToBone = true;
	bool bIncludeChildBones = true;
};

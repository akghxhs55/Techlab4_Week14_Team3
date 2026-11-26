#pragma once

#include "Editor/SkeletalPreviewEditor/SkeletalPreviewEditor.h"
#include "Animation/AnimMontageAsset.h"
#include "Math/Transform.h"

class UAnimGraphInstance;

class FAnimMontageEditor : public FSkeletalPreviewEditor
{
public:
	FAnimMontageEditor(UEditorEngine& InEditorEngine, FEditorManager& InEditorManager,
		const FString& InAssetPath, FAnimMontageAsset InAsset);
	~FAnimMontageEditor() override;

	const char* GetEditorTypeName() const override { return "AnimMontage"; }
	FAnimMontageAsset& GetMontageAsset() { return MontageAsset; }
	const FAnimMontageAsset& GetMontageAsset() const { return MontageAsset; }
	const FString& GetMessage() const { return Message; }

	bool Save();
	void PreviewPlay();
	void PreviewPause();
	void PreviewTogglePlayPause();
	void PreviewStop();
	void PreviewScrub(float InTime);
	void RefreshPreviewMontage();
	bool IsPreviewPlaying() const;
	bool IsPreviewPaused() const;
	float GetPreviewPosition() const;
	float GetMontageTotalLength() const;
	int32 GetSelectedNotifyIndex() const { return SelectedNotifyIndex; }
	void SetSelectedNotifyIndex(int32 InIndex) { SelectedNotifyIndex = InIndex; }

protected:
	bool InitializeResources() override;
	void ShutdownResources() override;
	void TickAfterWorld(float DeltaTime) override;
	TUniquePtr<IEditorTab> CreateTab() override;

private:
	void RebuildPreviewGraph();
	void RefreshAssetLength();
	void ApplyPreviewRootMotion();
	void ResetPreviewRootMotion();

	FAnimMontageAsset MontageAsset;
	TUniquePtr<UAnimGraphInstance> PreviewInstance;
	FString Message;
	FTransform PreviewBaseTransform;
	bool bHasPreviewBase = false;
	float CachedAssetLength = 0.0f;
	float ScrubPosition = 0.0f;
	int32 SelectedNotifyIndex = INDEX_NONE;
};

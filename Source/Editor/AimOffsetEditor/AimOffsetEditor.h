#pragma once

#include "Editor/SkeletalPreviewEditor/SkeletalPreviewEditor.h"
#include "Animation/AnimMontageAsset.h"

class UAnimGraphInstance;

class FAimOffsetEditor : public FSkeletalPreviewEditor
{
public:
	FAimOffsetEditor(UEditorEngine& InEditorEngine, FEditorManager& InEditorManager,
		const FString& InAssetPath, FAimOffsetAsset InAsset);
	~FAimOffsetEditor() override;

	const char* GetEditorTypeName() const override { return "AimOffset"; }
	FAimOffsetAsset& GetAimOffsetAsset() { return AimAsset; }
	const FAimOffsetAsset& GetAimOffsetAsset() const { return AimAsset; }
	const FString& GetMessage() const { return Message; }

	float PreviewPitch = 0.0f;
	float PreviewAlpha = 1.0f;

	bool Save();
	void RebuildPreview();
	void SyncPreviewParameters();

protected:
	bool InitializeResources() override;
	void ShutdownResources() override;
	TUniquePtr<IEditorTab> CreateTab() override;

private:
	FAimOffsetAsset AimAsset;
	TUniquePtr<UAnimGraphInstance> PreviewInstance;
	FString Message;
};

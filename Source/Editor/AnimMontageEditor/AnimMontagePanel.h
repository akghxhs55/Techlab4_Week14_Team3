#pragma once

#include "Editor/AssetEditor/AssetEditorPanel.h"
#include "AnimMontageEditor.h"
#include "Math/Vector.h"
#include <vector>

class FAnimMontagePanel : public TAssetEditorPanel<FAnimMontageEditor>
{
public:
	explicit FAnimMontagePanel(FAnimMontageEditor& InEditor)
		: TAssetEditorPanel<FAnimMontageEditor>(InEditor, "Montage Settings", "AnimMontagePanelClass")
	{
	}

protected:
	void DrawContent() override;

private:
	void RefreshSequences();
	void RefreshParticles();
	void DrawSequenceCombo(const char* Label, FString& InOutPath);
	void DrawParticleCombo(const char* Label, FString& InOutPath);
	void DrawBoneCombo(const char* Label, FString& InOutBone);
	void DrawNotifyDetails(FAnimMontageNotify& Notify, int32 NotifyIndex);

	std::vector<FString> SequencePaths;
	std::vector<FString> ParticlePaths;
	bool bInitialized = false;

	// 선택된 노티파이 속성 캐시
	int32 CachedNotifyIndex = INDEX_NONE;
	FString CachedClassName;
	FString CachedPropertiesJson;
	FString EditNamedNotify;
	FString EditParticlePath;
	FString EditBoneName;
	FString EditSecondBoneName;
	FString EditBaseParam = "RibbonBase";
	FString EditTipParam = "RibbonTip";
	FVector EditBoneOffset = FVector::ZeroVector;
	bool bEditFollowBoneRotation = true;
	bool bEditLoopDuringState = true;
};

class FAnimMontageTimelinePanel : public TAssetEditorPanel<FAnimMontageEditor>
{
public:
	explicit FAnimMontageTimelinePanel(FAnimMontageEditor& InEditor)
		: TAssetEditorPanel<FAnimMontageEditor>(InEditor, "Timeline", "AnimMontagePanelClass")
	{
	}

protected:
	void DrawContent() override;

private:
	enum class ETimelineDragMode
	{
		None,
		Scrub,
		MoveNotify,
		ResizeNotifyEnd
	};

	float ZoomScale = 1.0f;
	ETimelineDragMode DragMode = ETimelineDragMode::None;
	int32 DragNotifyIndex = INDEX_NONE;
	float DragStartMouseX = 0.0f;
	float DragStartValue = 0.0f;
	float ContextPopupTime = 0.0f;
	int32 ContextNotifyIndex = INDEX_NONE;
};

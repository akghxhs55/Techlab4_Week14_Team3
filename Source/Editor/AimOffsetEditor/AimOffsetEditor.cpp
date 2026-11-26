#include "EnginePCH.h"
#include "AimOffsetEditor.h"
#include "AimOffsetEditorTab.h"
#include "Animation/AnimGraphInstance.h"
#include "Asset/AssetManager.h"
#include "Asset/AssetUtils.h"
#include "Components/SkeletalMeshComponent.h"
#include "Render/SkeletalMesh.h"
#include "UObject/Casts.h"

FAimOffsetEditor::FAimOffsetEditor(UEditorEngine& InEditorEngine, FEditorManager& InEditorManager,
	const FString& InAssetPath, FAimOffsetAsset InAsset)
	: FSkeletalPreviewEditor(InEditorEngine, InEditorManager, InAssetPath)
	, AimAsset(std::move(InAsset))
{
}

FAimOffsetEditor::~FAimOffsetEditor() = default;

bool FAimOffsetEditor::InitializeResources()
{
	if (AimAsset.PreviewMeshPath.empty())
	{
		KRS_LOG(Warning, "AimOffset has no preview mesh: {}", GetAssetPath());
		return false;
	}

	const FString FilePath = FAssetUtils::ResolveAssetFilePath(AimAsset.PreviewMeshPath);
	FString Error;
	Mesh = Cast<USkeletalMesh>(UAssetManager::LoadFbxMesh(FilePath.empty() ? AimAsset.PreviewMeshPath : FilePath, Error));
	if (!Mesh || !InitializePreview())
	{
		return false;
	}

	PreviewInstance = TUniquePtr<UAnimGraphInstance>(NewObject<UAnimGraphInstance>(MeshComponent));
	RebuildPreview();
	return true;
}

void FAimOffsetEditor::ShutdownResources()
{
	if (MeshComponent && PreviewInstance && MeshComponent->GetAnimInstance() == PreviewInstance.get())
	{
		MeshComponent->SetAnimInstance(nullptr);
	}
	PreviewInstance.reset();
	FSkeletalPreviewEditor::ShutdownResources();
}

TUniquePtr<IEditorTab> FAimOffsetEditor::CreateTab()
{
	return MakeUnique<FAimOffsetEditorTab>(*this);
}

bool FAimOffsetEditor::Save()
{
	if (!AimAsset.Save(GetAssetPath(), Message))
	{
		return false;
	}
	Message = "AimOffset saved.";
	RebuildPreview();
	return true;
}

void FAimOffsetEditor::RebuildPreview()
{
	if (!PreviewInstance || !MeshComponent)
		return;

	PreviewInstance->ResetGraph();
	UAnimSequence* BaseSeq = !AimAsset.BaseSequencePath.empty()
		? PreviewInstance->LoadSequence(AimAsset.BaseSequencePath) : nullptr;
	if (!BaseSeq && !AimAsset.Samples.IsEmpty())
	{
		BaseSeq = PreviewInstance->LoadSequence(AimAsset.Samples[0].SequencePath);
	}

	if (BaseSeq)
	{
		const int32 BaseIdx = PreviewInstance->AddSequencePlayer(BaseSeq, true, 1.0f);
		const int32 AimIdx = PreviewInstance->AddAimOffset1D(BaseIdx, GetAssetPath(), "Pitch", "Alpha");
		if (AimIdx != INDEX_NONE)
		{
			PreviewInstance->SetRoot(AimIdx);
		}
		else
		{
			PreviewInstance->SetRoot(BaseIdx);
		}
	}
	MeshComponent->SetAnimInstance(PreviewInstance.get());
	SyncPreviewParameters();
}

void FAimOffsetEditor::SyncPreviewParameters()
{
	if (!PreviewInstance || !MeshComponent)
		return;
	PreviewInstance->SetFloat("Pitch", PreviewPitch);
	PreviewInstance->SetFloat("Alpha", PreviewAlpha);
	MeshComponent->RefreshAnimationPose();
}

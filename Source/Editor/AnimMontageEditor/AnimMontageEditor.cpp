#include "EnginePCH.h"
#include "AnimMontageEditor.h"
#include "AnimMontageEditorTab.h"
#include "Animation/AnimGraphInstance.h"
#include "Animation/AnimSequence.h"
#include "Asset/AssetManager.h"
#include "Asset/AssetUtils.h"
#include "Components/SkeletalMeshComponent.h"
#include "Render/SkeletalMesh.h"
#include "UObject/Casts.h"
#include <algorithm>

FAnimMontageEditor::FAnimMontageEditor(UEditorEngine& InEditorEngine, FEditorManager& InEditorManager,
	const FString& InAssetPath, FAnimMontageAsset InAsset)
	: FSkeletalPreviewEditor(InEditorEngine, InEditorManager, InAssetPath)
	, MontageAsset(std::move(InAsset))
{
}

FAnimMontageEditor::~FAnimMontageEditor() = default;

bool FAnimMontageEditor::InitializeResources()
{
	if (MontageAsset.PreviewMeshPath.empty())
	{
		KRS_LOG(Warning, "AnimMontage has no preview mesh: {}", GetAssetPath());
		return false;
	}

	const FString FilePath = FAssetUtils::ResolveAssetFilePath(MontageAsset.PreviewMeshPath);
	FString Error;
	Mesh = Cast<USkeletalMesh>(UAssetManager::LoadFbxMesh(FilePath.empty() ? MontageAsset.PreviewMeshPath : FilePath, Error));
	if (!Mesh || !InitializePreview())
	{
		return false;
	}

	PreviewInstance = TUniquePtr<UAnimGraphInstance>(NewObject<UAnimGraphInstance>(MeshComponent));
	RebuildPreviewGraph();
	RefreshAssetLength();
	MeshComponent->SetAnimInstance(PreviewInstance.get());
	PreviewBaseTransform = MeshComponent->GetTransform();
	bHasPreviewBase = true;
	return true;
}

void FAnimMontageEditor::RebuildPreviewGraph()
{
	if (!PreviewInstance)
		return;
	// 기존 그래프와 소유 시퀀스를 먼저 비운 뒤 시퀀스 로드
	PreviewInstance->ResetGraph();
	UAnimSequence* BaseSeq = !MontageAsset.SequencePath.empty()
		? PreviewInstance->LoadSequence(MontageAsset.SequencePath) : nullptr;
	if (!BaseSeq && !MontageAsset.Segments.IsEmpty())
	{
		BaseSeq = PreviewInstance->LoadSequence(MontageAsset.Segments[0].SequencePath);
	}

	if (BaseSeq)
	{
		const int32 PlayerIdx = PreviewInstance->AddSequencePlayer(BaseSeq, true, 1.0f);
		const int32 SlotIdx = PreviewInstance->AddSlotNode(PlayerIdx, MontageAsset.SlotName);
		PreviewInstance->SetRoot(SlotIdx);
	}
}

void FAnimMontageEditor::RefreshAssetLength()
{
	float MaxLen = 0.0f;
	if (PreviewInstance)
	{
		if (!MontageAsset.Segments.IsEmpty())
		{
			for (const FAnimMontageSegment& Seg : MontageAsset.Segments)
			{
				if (Seg.EndTime > Seg.StartTime)
				{
					MaxLen = std::max(MaxLen, Seg.EndTime);
					continue;
				}
				UAnimSequence* SegSeq = PreviewInstance->LoadSequence(Seg.SequencePath);
				if (SegSeq && SegSeq->GetDataModel())
				{
					const float Rate = Seg.PlayRate > 0.0f ? Seg.PlayRate : 1.0f;
					MaxLen = std::max(MaxLen, std::max(0.0f, Seg.StartTime) + SegSeq->GetDataModel()->GetPlayLength() / Rate);
				}
			}
		}
		else if (!MontageAsset.SequencePath.empty())
		{
			UAnimSequence* Seq = PreviewInstance->LoadSequence(MontageAsset.SequencePath);
			if (Seq && Seq->GetDataModel())
			{
				MaxLen = Seq->GetDataModel()->GetPlayLength();
			}
		}
	}
	for (const FAnimMontageSection& Sec : MontageAsset.Sections)
	{
		MaxLen = std::max(MaxLen, Sec.EndTime);
	}
	for (const FAnimMontageNotify& Notify : MontageAsset.Notifies)
	{
		MaxLen = std::max(MaxLen, Notify.Time + std::max(0.0f, Notify.Duration));
	}
	CachedAssetLength = std::max(MaxLen, 0.1f);
}

void FAnimMontageEditor::ShutdownResources()
{
	if (MeshComponent && PreviewInstance && MeshComponent->GetAnimInstance() == PreviewInstance.get())
	{
		MeshComponent->SetAnimInstance(nullptr);
	}
	PreviewInstance.reset();
	bHasPreviewBase = false;
	FSkeletalPreviewEditor::ShutdownResources();
}

void FAnimMontageEditor::TickAfterWorld(float DeltaTime)
{
	FSkeletalPreviewEditor::TickAfterWorld(DeltaTime);
	ApplyPreviewRootMotion();
}

void FAnimMontageEditor::ApplyPreviewRootMotion()
{
	if (!MeshComponent)
	{
		return;
	}
	if (!bHasPreviewBase)
	{
		PreviewBaseTransform = MeshComponent->GetTransform();
		bHasPreviewBase = true;
	}
	if (!MeshComponent->HasPendingRootMotion())
	{
		return;
	}

	const FTransform Delta = MeshComponent->ConsumeRootMotion();
	FTransform Moved = MeshComponent->GetTransform();
	Moved.Location = Moved.Location + Delta.Location;
	Moved.Rotation = (Delta.Rotation * Moved.Rotation).Normalized();

	// 몽타주 프리뷰 메시가 너무 멀어지면 기준 위치로 되돌린다
	const FBox Bounds = MeshComponent->CalcBounds();
	const float MeshHeight = Bounds.Max.Z - Bounds.Min.Z;
	const float MaxPreviewDistance = (MeshHeight > 0.0f ? MeshHeight : 1.0f) * 3.0f;
	const FVector FromBase = Moved.Location - PreviewBaseTransform.Location;
	if (FromBase.X * FromBase.X + FromBase.Y * FromBase.Y > MaxPreviewDistance * MaxPreviewDistance)
	{
		Moved = PreviewBaseTransform;
	}
	MeshComponent->SetTransform(Moved);
}

void FAnimMontageEditor::ResetPreviewRootMotion()
{
	if (!MeshComponent)
	{
		return;
	}
	MeshComponent->ConsumeRootMotion();
	if (bHasPreviewBase)
	{
		MeshComponent->SetTransform(PreviewBaseTransform);
	}
}

TUniquePtr<IEditorTab> FAnimMontageEditor::CreateTab()
{
	return MakeUnique<FAnimMontageEditorTab>(*this);
}

bool FAnimMontageEditor::Save()
{
	if (!MontageAsset.Save(GetAssetPath(), Message))
	{
		return false;
	}
	RefreshAssetLength();
	Message = "AnimMontage saved.";
	return true;
}

void FAnimMontageEditor::PreviewPlay()
{
	if (!PreviewInstance || !MeshComponent)
		return;
	if (PreviewInstance->IsPlayingMontage(MontageAsset.SlotName) && PreviewInstance->IsMontagePaused(MontageAsset.SlotName))
	{
		PreviewInstance->SetMontagePaused(false, MontageAsset.SlotName);
		return;
	}
	RefreshAssetLength();
	RebuildPreviewGraph();
	ResetPreviewRootMotion();
	MeshComponent->SetAnimInstance(PreviewInstance.get());
	PreviewInstance->PlayMontageFromAsset(GetAssetPath(), MontageAsset, 1.0f);
	if (ScrubPosition > 0.0f && ScrubPosition < CachedAssetLength)
	{
		PreviewInstance->SetMontagePosition(ScrubPosition, false, MontageAsset.SlotName);
	}
}

void FAnimMontageEditor::PreviewPause()
{
	if (!PreviewInstance)
		return;
	if (PreviewInstance->IsPlayingMontage(MontageAsset.SlotName))
	{
		ScrubPosition = PreviewInstance->GetMontagePosition(MontageAsset.SlotName);
		PreviewInstance->SetMontagePaused(true, MontageAsset.SlotName);
	}
}

void FAnimMontageEditor::PreviewTogglePlayPause()
{
	if (IsPreviewPlaying() && !IsPreviewPaused())
	{
		PreviewPause();
	}
	else
	{
		PreviewPlay();
	}
}

void FAnimMontageEditor::PreviewStop()
{
	if (PreviewInstance)
	{
		PreviewInstance->StopMontage(MontageAsset.SlotName, 0.0f);
	}
	ResetPreviewRootMotion();
	ScrubPosition = 0.0f;
}

void FAnimMontageEditor::PreviewScrub(float InTime)
{
	const float MaxLen = GetMontageTotalLength();
	ScrubPosition = std::clamp(InTime, 0.0f, MaxLen);
	if (!PreviewInstance || !MeshComponent)
		return;

	if (!PreviewInstance->IsPlayingMontage(MontageAsset.SlotName))
	{
		RebuildPreviewGraph();
		MeshComponent->SetAnimInstance(PreviewInstance.get());
		PreviewInstance->PlayMontageFromAsset(GetAssetPath(), MontageAsset, 1.0f);
	}
	PreviewInstance->SetMontagePosition(ScrubPosition, true, MontageAsset.SlotName);
	MeshComponent->RefreshAnimationPose();
}

void FAnimMontageEditor::RefreshPreviewMontage()
{
	RefreshAssetLength();
	if (!PreviewInstance || !MeshComponent)
		return;
	if (!PreviewInstance->IsPlayingMontage(MontageAsset.SlotName))
		return;
	const bool bWasPaused = PreviewInstance->IsMontagePaused(MontageAsset.SlotName);
	const float PrevPos = PreviewInstance->GetMontagePosition(MontageAsset.SlotName);
	PreviewInstance->PlayMontageFromAsset(GetAssetPath(), MontageAsset, 1.0f);
	PreviewInstance->SetMontagePosition(PrevPos, bWasPaused, MontageAsset.SlotName);
	if (bWasPaused)
	{
		MeshComponent->RefreshAnimationPose();
	}
}

bool FAnimMontageEditor::IsPreviewPlaying() const
{
	return PreviewInstance && PreviewInstance->IsPlayingMontage(MontageAsset.SlotName);
}

bool FAnimMontageEditor::IsPreviewPaused() const
{
	return PreviewInstance && PreviewInstance->IsMontagePaused(MontageAsset.SlotName);
}

float FAnimMontageEditor::GetPreviewPosition() const
{
	if (PreviewInstance && PreviewInstance->IsPlayingMontage(MontageAsset.SlotName))
	{
		return PreviewInstance->GetMontagePosition(MontageAsset.SlotName);
	}
	return ScrubPosition;
}

float FAnimMontageEditor::GetMontageTotalLength() const
{
	if (PreviewInstance && PreviewInstance->IsPlayingMontage(MontageAsset.SlotName))
	{
		const float LiveLen = PreviewInstance->GetMontageLength(MontageAsset.SlotName);
		if (LiveLen > 0.0f)
			return LiveLen;
	}
	return std::max(CachedAssetLength, 0.1f);
}

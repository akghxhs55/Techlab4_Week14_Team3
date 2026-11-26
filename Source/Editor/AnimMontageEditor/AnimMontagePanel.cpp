#include "EnginePCH.h"
#include "AnimMontagePanel.h"
#include "Editor/EditorUI/EditorStyle.h"
#include "Animation/AnimNotifyState.h"
#include "Animation/AnimSequenceFile.h"
#include "Asset/AssetUtils.h"
#include "Core/Misc/Paths.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/StructuredArchive.h"
#include "Serialization/StructuredArchiveSlots.h"
#include "Serialization/Formatter/JsonArchiveInputFormatter.h"
#include "Serialization/Formatter/JsonArchiveOutputFormatter.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace
{
	constexpr float MontageRulerHeight = 24.0f;
	constexpr float MontageRowHeight = 28.0f;
	constexpr float MontageNotifyRowHeight = 36.0f;
	constexpr float MontageHeaderWidth = 100.0f;
	constexpr float MontageDiamondRadius = 6.0f;
	constexpr float MontageEndHandleWidth = 6.0f;

	void LoadParticlePropertiesFromJson(const FString& JsonText, FString& OutParticlePath, FString& OutBoneName,
		FString& OutSecondBoneName, FString& OutBaseParam, FString& OutTipParam, FVector& OutOffset,
		bool& bOutFollowRotation, bool& bOutLoopDuringState)
	{
		UAnimNotifyState_ParticleEffect* TempObj = NewObject<UAnimNotifyState_ParticleEffect>();
		if (!TempObj)
			return;
		if (!JsonText.empty())
		{
			TArray<uint8> Bytes;
			Bytes.SetNum(static_cast<int32>(JsonText.size()));
			std::memcpy(Bytes.GetData(), JsonText.data(), JsonText.size());
			FMemoryReader Reader(Bytes, true);
			FJsonArchiveInputFormatter Formatter(Reader);
			if (!Reader.IsError())
			{
				FStructuredArchive Archive(Formatter);
				TempObj->GetClass()->SerializeTaggedProperties(Archive.Open(), TempObj);
			}
		}
		OutParticlePath = TempObj->ParticlePath;
		OutBoneName = TempObj->BoneName;
		OutSecondBoneName = TempObj->SecondBoneName;
		OutBaseParam = TempObj->BaseParameterName;
		OutTipParam = TempObj->TipParameterName;
		OutOffset = TempObj->BoneOffset;
		bOutFollowRotation = TempObj->bFollowBoneRotation;
		bOutLoopDuringState = TempObj->bLoopDuringState;
		delete TempObj;
	}

	FString SaveParticlePropertiesToJson(const FString& ParticlePath, const FString& BoneName,
		const FString& SecondBoneName, const FString& BaseParam, const FString& TipParam,
		const FVector& Offset, bool bFollowRotation, bool bLoopDuringState)
	{
		UAnimNotifyState_ParticleEffect* TempObj = NewObject<UAnimNotifyState_ParticleEffect>();
		if (!TempObj)
			return "{}";
		TempObj->ParticlePath = ParticlePath;
		TempObj->BoneName = BoneName;
		TempObj->SecondBoneName = SecondBoneName;
		TempObj->BaseParameterName = BaseParam;
		TempObj->TipParameterName = TipParam;
		TempObj->BoneOffset = Offset;
		TempObj->bFollowBoneRotation = bFollowRotation;
		TempObj->bLoopDuringState = bLoopDuringState;

		TArray<uint8> Bytes;
		FMemoryWriter Writer(Bytes);
		{
			FJsonArchiveOutputFormatter Formatter(Writer);
			FStructuredArchive Archive(Formatter);
			TempObj->GetClass()->SerializeTaggedProperties(Archive.Open(), TempObj);
		}
		delete TempObj;
		if (Bytes.IsEmpty())
			return "{}";
		return FString(reinterpret_cast<const char*>(Bytes.GetData()), Bytes.Num());
	}

	void LoadNamedNotifyFromJson(const FString& JsonText, FString& OutNotifyName)
	{
		UAnimNotifyState_Named* TempObj = NewObject<UAnimNotifyState_Named>();
		if (!TempObj)
			return;
		if (!JsonText.empty())
		{
			TArray<uint8> Bytes;
			Bytes.SetNum(static_cast<int32>(JsonText.size()));
			std::memcpy(Bytes.GetData(), JsonText.data(), JsonText.size());
			FMemoryReader Reader(Bytes, true);
			FJsonArchiveInputFormatter Formatter(Reader);
			if (!Reader.IsError())
			{
				FStructuredArchive Archive(Formatter);
				TempObj->GetClass()->SerializeTaggedProperties(Archive.Open(), TempObj);
			}
		}
		OutNotifyName = TempObj->NotifyName;
		delete TempObj;
	}

	FString SaveNamedNotifyToJson(const FString& NotifyName)
	{
		UAnimNotifyState_Named* TempObj = NewObject<UAnimNotifyState_Named>();
		if (!TempObj)
			return "{}";
		TempObj->NotifyName = NotifyName;
		TArray<uint8> Bytes;
		FMemoryWriter Writer(Bytes);
		{
			FJsonArchiveOutputFormatter Formatter(Writer);
			FStructuredArchive Archive(Formatter);
			TempObj->GetClass()->SerializeTaggedProperties(Archive.Open(), TempObj);
		}
		delete TempObj;
		if (Bytes.IsEmpty())
			return "{}";
		return FString(reinterpret_cast<const char*>(Bytes.GetData()), Bytes.Num());
	}

	float ComputeMontageTickStep(float PixelsPerSecond)
	{
		const float TargetPx = 70.0f;
		const float RawSec = TargetPx / std::max(1.0f, PixelsPerSecond);
		const float Steps[] = { 0.05f, 0.1f, 0.25f, 0.5f, 1.0f, 2.0f, 5.0f, 10.0f };
		for (float Step : Steps)
		{
			if (Step >= RawSec)
				return Step;
		}
		return 10.0f;
	}

	FString BaseFilenameOf(const FString& AssetPath)
	{
		if (AssetPath.empty())
			return "None";
		return std::filesystem::path(AssetPath).filename().generic_string();
	}
}

void FAnimMontagePanel::RefreshSequences()
{
	SequencePaths.clear();
	bInitialized = true;

	const FString& PreviewPath = Editor.GetMontageAsset().PreviewMeshPath;
	FString Resolved = FAssetUtils::ResolveAssetFilePath(PreviewPath);
	if (Resolved.empty())
		Resolved = PreviewPath;
	std::filesystem::path Dir = std::filesystem::path(Resolved).parent_path();
	if (Dir.empty())
		Dir = FPaths::ProjectContentDir();

	std::error_code Ec;
	for (const std::filesystem::directory_entry& Entry : std::filesystem::recursive_directory_iterator(Dir, Ec))
	{
		if (!Entry.is_regular_file(Ec))
			continue;
		const FString Path = Entry.path().generic_string();
		if (AnimSequenceFile::IsAnimSequencePath(Path))
		{
			SequencePaths.push_back(FAssetUtils::GetAssetKey(Path));
		}
	}
	std::sort(SequencePaths.begin(), SequencePaths.end());
}

void FAnimMontagePanel::RefreshParticles()
{
	ParticlePaths.clear();
	std::error_code Ec;
	const std::filesystem::path ContentDir = FPaths::ProjectContentDir();
	for (const std::filesystem::directory_entry& Entry : std::filesystem::recursive_directory_iterator(ContentDir, Ec))
	{
		if (!Entry.is_regular_file(Ec))
			continue;
		if (Entry.path().extension() == ".particle")
		{
			ParticlePaths.push_back(FAssetUtils::GetAssetKey(Entry.path().generic_string()));
		}
	}
	std::sort(ParticlePaths.begin(), ParticlePaths.end());
}

void FAnimMontagePanel::DrawSequenceCombo(const char* Label, FString& InOutPath)
{
	if (!bInitialized)
	{
		RefreshSequences();
		RefreshParticles();
	}

	const FString Display = BaseFilenameOf(InOutPath);
	if (ImGui::BeginCombo(Label, Display.c_str()))
	{
		if (ImGui::Selectable("None", InOutPath.empty()))
		{
			InOutPath.clear();
			Editor.RefreshPreviewMontage();
		}
		for (const FString& Path : SequencePaths)
		{
			const bool bSelected = (InOutPath == Path);
			if (ImGui::Selectable(Path.c_str(), bSelected))
			{
				InOutPath = Path;
				Editor.RefreshPreviewMontage();
			}
		}
		ImGui::EndCombo();
	}
}

void FAnimMontagePanel::DrawParticleCombo(const char* Label, FString& InOutPath)
{
	if (!bInitialized)
	{
		RefreshSequences();
		RefreshParticles();
	}

	const FString Display = InOutPath.empty() ? FString("None") : BaseFilenameOf(InOutPath);
	if (ImGui::BeginCombo(Label, Display.c_str()))
	{
		if (ImGui::Selectable("None", InOutPath.empty()))
		{
			InOutPath.clear();
		}
		for (const FString& Path : ParticlePaths)
		{
			const bool bSelected = (InOutPath == Path);
			if (ImGui::Selectable(Path.c_str(), bSelected))
			{
				InOutPath = Path;
			}
		}
		ImGui::EndCombo();
	}
}

void FAnimMontagePanel::DrawBoneCombo(const char* Label, FString& InOutBone)
{
	const TArray<FEditorBoneItem>& Bones = Editor.GetBoneItems();
	const char* Preview = InOutBone.empty() ? "Root / None" : InOutBone.c_str();
	if (ImGui::BeginCombo(Label, Preview))
	{
		if (ImGui::Selectable("None", InOutBone.empty()))
		{
			InOutBone.clear();
		}
		for (const FEditorBoneItem& Bone : Bones)
		{
			const bool bSelected = (InOutBone == Bone.Name);
			if (ImGui::Selectable(Bone.Name.c_str(), bSelected))
			{
				InOutBone = Bone.Name;
			}
		}
		ImGui::EndCombo();
	}
}

void FAnimMontagePanel::DrawNotifyDetails(FAnimMontageNotify& Notify, int32 NotifyIndex)
{
	if (CachedNotifyIndex != NotifyIndex || CachedClassName != Notify.ClassName || CachedPropertiesJson != Notify.Properties)
	{
		CachedNotifyIndex = NotifyIndex;
		CachedClassName = Notify.ClassName;
		CachedPropertiesJson = Notify.Properties;
		if (Notify.ClassName == "UAnimNotifyState_ParticleEffect")
		{
			LoadParticlePropertiesFromJson(Notify.Properties, EditParticlePath, EditBoneName,
				EditSecondBoneName, EditBaseParam, EditTipParam, EditBoneOffset,
				bEditFollowBoneRotation, bEditLoopDuringState);
		}
		else if (Notify.ClassName == "UAnimNotifyState_Named")
		{
			LoadNamedNotifyFromJson(Notify.Properties, EditNamedNotify);
		}
	}

	if (Notify.ClassName == "UAnimNotifyState_ParticleEffect")
	{
		bool bChanged = false;
		const FString PrevParticle = EditParticlePath;
		DrawParticleCombo("Particle", EditParticlePath);
		if (EditParticlePath != PrevParticle)
			bChanged = true;

		char PathBuf[256] = {};
		std::snprintf(PathBuf, sizeof(PathBuf), "%s", EditParticlePath.c_str());
		if (ImGui::InputText("Particle Path", PathBuf, sizeof(PathBuf)))
		{
			EditParticlePath = PathBuf;
			bChanged = true;
		}

		const FString PrevBone = EditBoneName;
		DrawBoneCombo("Bone", EditBoneName);
		if (EditBoneName != PrevBone)
			bChanged = true;

		const FString PrevSecondBone = EditSecondBoneName;
		DrawBoneCombo("Second Bone", EditSecondBoneName);
		if (EditSecondBoneName != PrevSecondBone)
			bChanged = true;

		char BaseBuf[128] = {};
		char TipBuf[128] = {};
		std::snprintf(BaseBuf, sizeof(BaseBuf), "%s", EditBaseParam.c_str());
		std::snprintf(TipBuf, sizeof(TipBuf), "%s", EditTipParam.c_str());
		if (ImGui::InputText("Base Param", BaseBuf, sizeof(BaseBuf)))
		{
			EditBaseParam = BaseBuf;
			bChanged = true;
		}
		if (ImGui::InputText("Tip Param", TipBuf, sizeof(TipBuf)))
		{
			EditTipParam = TipBuf;
			bChanged = true;
		}

		float OffsetArr[3] = { EditBoneOffset.X, EditBoneOffset.Y, EditBoneOffset.Z };
		if (ImGui::DragFloat3("Bone Offset", OffsetArr, 0.5f))
		{
			EditBoneOffset = FVector(OffsetArr[0], OffsetArr[1], OffsetArr[2]);
			bChanged = true;
		}
		if (ImGui::Checkbox("Follow Bone Rotation", &bEditFollowBoneRotation))
			bChanged = true;
		if (ImGui::Checkbox("Loop During State", &bEditLoopDuringState))
			bChanged = true;

		if (bChanged)
		{
			Notify.Properties = SaveParticlePropertiesToJson(EditParticlePath, EditBoneName,
				EditSecondBoneName, EditBaseParam, EditTipParam, EditBoneOffset,
				bEditFollowBoneRotation, bEditLoopDuringState);
			CachedPropertiesJson = Notify.Properties;
			Editor.RefreshPreviewMontage();
		}
	}
	else if (Notify.ClassName == "UAnimNotifyState_Named")
	{
		char NamedBuf[128] = {};
		std::snprintf(NamedBuf, sizeof(NamedBuf), "%s", EditNamedNotify.c_str());
		if (ImGui::InputText("Notify State Name", NamedBuf, sizeof(NamedBuf)))
		{
			EditNamedNotify = NamedBuf;
			Notify.Properties = SaveNamedNotifyToJson(EditNamedNotify);
			CachedPropertiesJson = Notify.Properties;
			Editor.RefreshPreviewMontage();
		}
	}
}

void FAnimMontagePanel::DrawContent()
{
	if (ImGui::Button("Save"))
		Editor.Save();
	ImGui::SameLine();
	if (ImGui::Button("Play Montage"))
		Editor.PreviewPlay();
	ImGui::SameLine();
	if (ImGui::Button("Stop Montage"))
		Editor.PreviewStop();
	ImGui::SameLine();
	if (ImGui::Button("Refresh Assets"))
	{
		RefreshSequences();
		RefreshParticles();
	}

	const FString& Message = Editor.GetMessage();
	if (!Message.empty())
		ImGui::TextWrapped("%s", Message.c_str());

	FAnimMontageAsset& Asset = Editor.GetMontageAsset();
	ImGui::SeparatorText("Montage");

	char SlotBuf[128] = {};
	std::snprintf(SlotBuf, sizeof(SlotBuf), "%s", Asset.SlotName.c_str());
	if (ImGui::InputText("Slot Name", SlotBuf, sizeof(SlotBuf)))
		Asset.SlotName = SlotBuf;

	DrawSequenceCombo("Sequence", Asset.SequencePath);
	ImGui::DragFloat("Play Rate", &Asset.PlayRate, 0.01f, 0.05f, 5.0f);
	ImGui::DragFloat("Blend In Time", &Asset.BlendInTime, 0.01f, 0.0f, 2.0f);
	ImGui::DragFloat("Blend Out Time", &Asset.BlendOutTime, 0.01f, 0.0f, 2.0f);
	ImGui::Checkbox("Loop", &Asset.bLoop);

	if (ImGui::CollapsingHeader("Segments", ImGuiTreeNodeFlags_DefaultOpen))
	{
		if (ImGui::Button("Add Segment"))
		{
			FAnimMontageSegment Seg;
			Seg.SequencePath = Asset.SequencePath;
			Asset.Segments.Add(Seg);
			Editor.RefreshPreviewMontage();
		}
		int32 RemoveIdx = -1;
		for (int32 Idx = 0; Idx < Asset.Segments.Num(); ++Idx)
		{
			ImGui::PushID(Idx);
			FAnimMontageSegment& Seg = Asset.Segments[Idx];
			DrawSequenceCombo("Seq", Seg.SequencePath);
			if (ImGui::DragFloat("Start", &Seg.StartTime, 0.01f, 0.0f, 100.0f))
				Editor.RefreshPreviewMontage();
			if (ImGui::DragFloat("End", &Seg.EndTime, 0.01f, 0.0f, 100.0f))
				Editor.RefreshPreviewMontage();
			if (ImGui::DragFloat("Offset", &Seg.AnimStartOffset, 0.01f, 0.0f, 100.0f))
				Editor.RefreshPreviewMontage();
			if (ImGui::DragFloat("Rate", &Seg.PlayRate, 0.01f, 0.05f, 5.0f))
				Editor.RefreshPreviewMontage();
			if (ImGui::SmallButton("Remove Segment"))
				RemoveIdx = Idx;
			ImGui::Separator();
			ImGui::PopID();
		}
		if (RemoveIdx >= 0)
		{
			Asset.Segments.RemoveAt(RemoveIdx, 1);
			Editor.RefreshPreviewMontage();
		}
	}

	if (ImGui::CollapsingHeader("Sections"))
	{
		if (ImGui::Button("Add Section"))
		{
			FAnimMontageSection Sec;
			Sec.Name = "Section_" + std::to_string(Asset.Sections.Num());
			Sec.StartTime = 0.0f;
			Sec.EndTime = 1.0f;
			Asset.Sections.Add(Sec);
			Editor.RefreshPreviewMontage();
		}
		int32 RemoveIdx = -1;
		for (int32 Idx = 0; Idx < Asset.Sections.Num(); ++Idx)
		{
			ImGui::PushID(1000 + Idx);
			FAnimMontageSection& Sec = Asset.Sections[Idx];
			char NameBuf[128] = {};
			char NextBuf[128] = {};
			std::snprintf(NameBuf, sizeof(NameBuf), "%s", Sec.Name.c_str());
			std::snprintf(NextBuf, sizeof(NextBuf), "%s", Sec.NextSection.c_str());
			if (ImGui::InputText("Name", NameBuf, sizeof(NameBuf)))
				Sec.Name = NameBuf;
			if (ImGui::DragFloat("Start", &Sec.StartTime, 0.01f, 0.0f, 100.0f))
				Editor.RefreshPreviewMontage();
			if (ImGui::DragFloat("End", &Sec.EndTime, 0.01f, 0.01f, 100.0f))
				Editor.RefreshPreviewMontage();
			if (ImGui::InputText("Next", NextBuf, sizeof(NextBuf)))
				Sec.NextSection = NextBuf;
			if (ImGui::SmallButton("Remove Section"))
				RemoveIdx = Idx;
			ImGui::Separator();
			ImGui::PopID();
		}
		if (RemoveIdx >= 0)
		{
			Asset.Sections.RemoveAt(RemoveIdx, 1);
			Editor.RefreshPreviewMontage();
		}
	}

	if (ImGui::CollapsingHeader("Notifies", ImGuiTreeNodeFlags_DefaultOpen))
	{
		if (ImGui::Button("Add Notify"))
		{
			FAnimMontageNotify Notify;
			Notify.Name = "Notify";
			Notify.Time = Editor.GetPreviewPosition();
			Asset.Notifies.Add(Notify);
			Editor.SetSelectedNotifyIndex(Asset.Notifies.Num() - 1);
		}
		ImGui::SameLine();
		if (ImGui::Button("Add Particle State"))
		{
			FAnimMontageNotify Notify;
			Notify.Name = "ParticleEffect";
			Notify.ClassName = "UAnimNotifyState_ParticleEffect";
			Notify.Time = Editor.GetPreviewPosition();
			Notify.Duration = 0.2f;
			Asset.Notifies.Add(Notify);
			Editor.SetSelectedNotifyIndex(Asset.Notifies.Num() - 1);
		}

		int32 RemoveIdx = -1;
		const int32 SelectedIdx = Editor.GetSelectedNotifyIndex();
		for (int32 Idx = 0; Idx < Asset.Notifies.Num(); ++Idx)
		{
			ImGui::PushID(2000 + Idx);
			FAnimMontageNotify& Notify = Asset.Notifies[Idx];
			const bool bIsSelected = (Idx == SelectedIdx);
			const FString HeaderLabel = (Notify.Name.empty() ? FString("Notify") : Notify.Name)
				+ " (" + (Notify.Duration > 0.0f ? FString("State") : FString("Notify")) + ")##Item" + std::to_string(Idx);
			if (ImGui::Selectable(HeaderLabel.c_str(), bIsSelected))
			{
				Editor.SetSelectedNotifyIndex(Idx);
			}

			if (bIsSelected)
			{
				ImGui::Indent(8.0f);
				char NameBuf[128] = {};
				char ClassBuf[128] = {};
				std::snprintf(NameBuf, sizeof(NameBuf), "%s", Notify.Name.c_str());
				std::snprintf(ClassBuf, sizeof(ClassBuf), "%s", Notify.ClassName.c_str());
				if (ImGui::InputText("Name", NameBuf, sizeof(NameBuf)))
					Notify.Name = NameBuf;

				const char* PresetLabel = Notify.ClassName.empty() ? "None (Event)" : Notify.ClassName.c_str();
				if (ImGui::BeginCombo("Class Preset", PresetLabel))
				{
					if (ImGui::Selectable("None (Event)", Notify.ClassName.empty()))
					{
						Notify.ClassName.clear();
						Notify.Duration = 0.0f;
					}
					if (ImGui::Selectable("UAnimNotifyState_ParticleEffect", Notify.ClassName == "UAnimNotifyState_ParticleEffect"))
					{
						Notify.ClassName = "UAnimNotifyState_ParticleEffect";
						if (Notify.Duration <= 0.0f)
							Notify.Duration = 0.2f;
					}
					if (ImGui::Selectable("UAnimNotifyState_Named", Notify.ClassName == "UAnimNotifyState_Named"))
					{
						Notify.ClassName = "UAnimNotifyState_Named";
						if (Notify.Duration <= 0.0f)
							Notify.Duration = 0.2f;
					}
					ImGui::EndCombo();
				}

				if (ImGui::InputText("Class", ClassBuf, sizeof(ClassBuf)))
					Notify.ClassName = ClassBuf;
				if (ImGui::DragFloat("Time", &Notify.Time, 0.01f, 0.0f, 100.0f))
					Editor.RefreshPreviewMontage();
				if (ImGui::DragFloat("Duration", &Notify.Duration, 0.01f, 0.0f, 100.0f))
					Editor.RefreshPreviewMontage();

				DrawNotifyDetails(Notify, Idx);

				if (ImGui::SmallButton("Remove Notify"))
					RemoveIdx = Idx;
				ImGui::Unindent(8.0f);
				ImGui::Separator();
			}
			ImGui::PopID();
		}
		if (RemoveIdx >= 0)
		{
			Asset.Notifies.RemoveAt(RemoveIdx, 1);
			if (Editor.GetSelectedNotifyIndex() == RemoveIdx)
				Editor.SetSelectedNotifyIndex(INDEX_NONE);
			else if (Editor.GetSelectedNotifyIndex() > RemoveIdx)
				Editor.SetSelectedNotifyIndex(Editor.GetSelectedNotifyIndex() - 1);
			CachedNotifyIndex = INDEX_NONE;
			Editor.RefreshPreviewMontage();
		}
	}
}

void FAnimMontageTimelinePanel::DrawContent()
{
	FAnimMontageAsset& Asset = Editor.GetMontageAsset();
	const float TotalLength = Editor.GetMontageTotalLength();
	const float CurrentPos = Editor.GetPreviewPosition();
	const bool bPlaying = Editor.IsPreviewPlaying();
	const bool bPaused = Editor.IsPreviewPaused();

	if (ImGui::Button("Save"))
	{
		Editor.Save();
	}
	ImGui::SameLine();
	const char* PlayLabel = (bPlaying && !bPaused) ? "Pause" : "Play";
	if (ImGui::Button(PlayLabel))
	{
		Editor.PreviewTogglePlayPause();
	}
	ImGui::SameLine();
	if (ImGui::Button("Stop"))
	{
		Editor.PreviewStop();
	}
	ImGui::SameLine();
	ImGui::Checkbox("Loop", &Asset.bLoop);
	ImGui::SameLine();
	ImGui::Text("| %.3fs / %.3fs", CurrentPos, TotalLength);
	ImGui::SameLine();
	ImGui::SetNextItemWidth(110.0f);
	ImGui::SliderFloat("Zoom", &ZoomScale, 1.0f, 8.0f, "%.1fx");
	ImGui::SameLine();
	if (ImGui::Button("+ Particle Notify"))
	{
		FAnimMontageNotify Notify;
		Notify.Name = "ParticleEffect";
		Notify.ClassName = "UAnimNotifyState_ParticleEffect";
		Notify.Time = CurrentPos;
		Notify.Duration = std::min(0.25f, std::max(0.05f, TotalLength - CurrentPos));
		Asset.Notifies.Add(Notify);
		Editor.SetSelectedNotifyIndex(Asset.Notifies.Num() - 1);
		Editor.RefreshPreviewMontage();
	}
	ImGui::SameLine();
	if (ImGui::Button("+ Notify"))
	{
		FAnimMontageNotify Notify;
		Notify.Name = "Notify";
		Notify.Time = CurrentPos;
		Asset.Notifies.Add(Notify);
		Editor.SetSelectedNotifyIndex(Asset.Notifies.Num() - 1);
		Editor.RefreshPreviewMontage();
	}

	ImGui::Separator();

	const ImVec2 Avail = ImGui::GetContentRegionAvail();
	if (Avail.x <= MontageHeaderWidth + 20.0f || Avail.y <= 40.0f)
		return;

	ImGui::BeginChild("MontageTimelineScroll", ImVec2(0.0f, 0.0f), false, ImGuiWindowFlags_HorizontalScrollbar);

	const float TrackViewWidth = std::max(100.0f, Avail.x - MontageHeaderWidth - 16.0f);
	const float TimelineWidth = TrackViewWidth * ZoomScale;
	const float PixelsPerSec = TimelineWidth / std::max(0.1f, TotalLength);
	const float TotalRowsHeight = MontageRulerHeight + MontageRowHeight * 2.0f + MontageNotifyRowHeight;

	const ImVec2 CanvasOrigin = ImGui::GetCursorScreenPos();
	const ImVec2 TrackOrigin(CanvasOrigin.x + MontageHeaderWidth, CanvasOrigin.y);
	ImDrawList* DrawList = ImGui::GetWindowDrawList();

	// 좌측 트랙 헤더 배경
	DrawList->AddRectFilled(
		CanvasOrigin,
		ImVec2(TrackOrigin.x, CanvasOrigin.y + TotalRowsHeight),
		EditorColor::BgPanel);

	// 우측 트랙 배경
	DrawList->AddRectFilled(
		ImVec2(TrackOrigin.x, CanvasOrigin.y),
		ImVec2(TrackOrigin.x + TimelineWidth, CanvasOrigin.y + MontageRulerHeight),
		EditorColor::BgElevated);
	DrawList->AddRectFilled(
		ImVec2(TrackOrigin.x, CanvasOrigin.y + MontageRulerHeight),
		ImVec2(TrackOrigin.x + TimelineWidth, CanvasOrigin.y + MontageRulerHeight + MontageRowHeight),
		EditorColor::BgElevatedAlt);
	DrawList->AddRectFilled(
		ImVec2(TrackOrigin.x, CanvasOrigin.y + MontageRulerHeight + MontageRowHeight),
		ImVec2(TrackOrigin.x + TimelineWidth, CanvasOrigin.y + MontageRulerHeight + MontageRowHeight * 2.0f),
		EditorColor::BgElevated);
	DrawList->AddRectFilled(
		ImVec2(TrackOrigin.x, CanvasOrigin.y + MontageRulerHeight + MontageRowHeight * 2.0f),
		ImVec2(TrackOrigin.x + TimelineWidth, CanvasOrigin.y + TotalRowsHeight),
		EditorColor::BgElevatedAlt);

	const float SegY = CanvasOrigin.y + MontageRulerHeight;
	const float SecY = SegY + MontageRowHeight;
	const float NotY = SecY + MontageRowHeight;

	// 트랙 구분선 및 헤더 이름
	DrawList->AddLine(ImVec2(TrackOrigin.x, CanvasOrigin.y), ImVec2(TrackOrigin.x, CanvasOrigin.y + TotalRowsHeight), IM_COL32(70, 74, 84, 255));
	DrawList->AddLine(ImVec2(CanvasOrigin.x, SegY), ImVec2(TrackOrigin.x + TimelineWidth, SegY), IM_COL32(60, 64, 74, 255));
	DrawList->AddLine(ImVec2(CanvasOrigin.x, SecY), ImVec2(TrackOrigin.x + TimelineWidth, SecY), IM_COL32(60, 64, 74, 255));
	DrawList->AddLine(ImVec2(CanvasOrigin.x, NotY), ImVec2(TrackOrigin.x + TimelineWidth, NotY), IM_COL32(60, 64, 74, 255));

	DrawList->AddText(ImVec2(CanvasOrigin.x + 8.0f, CanvasOrigin.y + 4.0f), EditorColor::TextSecondary, "Time");
	DrawList->AddText(ImVec2(CanvasOrigin.x + 8.0f, SegY + 6.0f), EditorColor::TextPrimary, "Segments");
	DrawList->AddText(ImVec2(CanvasOrigin.x + 8.0f, SecY + 6.0f), EditorColor::TextPrimary, "Sections");
	DrawList->AddText(ImVec2(CanvasOrigin.x + 8.0f, NotY + 10.0f), EditorColor::TextPrimary, "Notifies");

	// 눈금 그리기
	const float TickStep = ComputeMontageTickStep(PixelsPerSec);
	const int32 TickCount = static_cast<int32>(std::ceil(TotalLength / TickStep)) + 1;
	for (int32 TickIdx = 0; TickIdx <= TickCount; ++TickIdx)
	{
		const float TickTime = TickIdx * TickStep;
		if (TickTime > TotalLength + 0.0001f)
			break;
		const float X = TrackOrigin.x + TickTime * PixelsPerSec;
		DrawList->AddLine(ImVec2(X, CanvasOrigin.y + MontageRulerHeight * 0.55f), ImVec2(X, CanvasOrigin.y + MontageRulerHeight), IM_COL32(150, 155, 170, 220));
		DrawList->AddLine(ImVec2(X, SegY), ImVec2(X, CanvasOrigin.y + TotalRowsHeight), IM_COL32(55, 58, 66, 120));
		char LabelBuf[32] = {};
		std::snprintf(LabelBuf, sizeof(LabelBuf), "%.2fs", TickTime);
		DrawList->AddText(ImVec2(X + 3.0f, CanvasOrigin.y + 3.0f), EditorColor::TextSecondary, LabelBuf);
	}

	// 세그먼트 막대
	if (!Asset.Segments.IsEmpty())
	{
		for (const FAnimMontageSegment& Seg : Asset.Segments)
		{
			const float StartX = TrackOrigin.x + std::max(0.0f, Seg.StartTime) * PixelsPerSec;
			const float EndTime = Seg.EndTime > Seg.StartTime ? Seg.EndTime : TotalLength;
			const float EndX = TrackOrigin.x + std::min(TotalLength, EndTime) * PixelsPerSec;
			DrawList->AddRectFilled(ImVec2(StartX, SegY + 3.0f), ImVec2(std::max(StartX + 4.0f, EndX), SegY + MontageRowHeight - 3.0f), IM_COL32(58, 138, 86, 220), 3.0f);
			DrawList->AddRect(ImVec2(StartX, SegY + 3.0f), ImVec2(std::max(StartX + 4.0f, EndX), SegY + MontageRowHeight - 3.0f), IM_COL32(115, 210, 145, 255), 3.0f);
			DrawList->AddText(ImVec2(StartX + 6.0f, SegY + 6.0f), IM_COL32(245, 250, 245, 255), BaseFilenameOf(Seg.SequencePath).c_str());
		}
	}
	else if (!Asset.SequencePath.empty())
	{
		const float EndX = TrackOrigin.x + TotalLength * PixelsPerSec;
		DrawList->AddRectFilled(ImVec2(TrackOrigin.x, SegY + 3.0f), ImVec2(EndX, SegY + MontageRowHeight - 3.0f), IM_COL32(58, 138, 86, 220), 3.0f);
		DrawList->AddText(ImVec2(TrackOrigin.x + 6.0f, SegY + 6.0f), IM_COL32(245, 250, 245, 255), BaseFilenameOf(Asset.SequencePath).c_str());
	}

	// 섹션 막대
	for (const FAnimMontageSection& Sec : Asset.Sections)
	{
		const float StartX = TrackOrigin.x + std::max(0.0f, Sec.StartTime) * PixelsPerSec;
		const float EndX = TrackOrigin.x + std::min(TotalLength, Sec.EndTime) * PixelsPerSec;
		DrawList->AddRectFilled(ImVec2(StartX, SecY + 3.0f), ImVec2(std::max(StartX + 6.0f, EndX), SecY + MontageRowHeight - 3.0f), IM_COL32(82, 108, 178, 220), 3.0f);
		DrawList->AddRect(ImVec2(StartX, SecY + 3.0f), ImVec2(std::max(StartX + 6.0f, EndX), SecY + MontageRowHeight - 3.0f), IM_COL32(140, 170, 240, 255), 3.0f);
		DrawList->AddText(ImVec2(StartX + 6.0f, SecY + 6.0f), IM_COL32(240, 245, 255, 255), Sec.Name.c_str());
	}

	// 노티파이 렌더링 및 히트 테스트 준비
	const int32 SelectedNotifyIdx = Editor.GetSelectedNotifyIndex();
	const ImVec2 MousePos = ImGui::GetMousePos();
	int32 HoveredNotifyIdx = INDEX_NONE;
	bool bHoveredRightHandle = false;

	for (int32 Idx = 0; Idx < Asset.Notifies.Num(); ++Idx)
	{
		const FAnimMontageNotify& Notify = Asset.Notifies[Idx];
		const bool bIsState = Notify.Duration > 0.0f;
		const bool bIsSelected = (Idx == SelectedNotifyIdx);
		const float StartX = TrackOrigin.x + std::clamp(Notify.Time, 0.0f, TotalLength) * PixelsPerSec;
		const FString DisplayName = Notify.Name.empty() ? Notify.ClassName : Notify.Name;

		if (bIsState)
		{
			const float BarWidth = std::max(8.0f, Notify.Duration * PixelsPerSec);
			const float EndX = StartX + BarWidth;
			const ImVec2 BarMin(StartX, NotY + 5.0f);
			const ImVec2 BarMax(EndX, NotY + MontageNotifyRowHeight - 5.0f);
			const ImU32 FillCol = (Notify.ClassName == "UAnimNotifyState_ParticleEffect")
				? IM_COL32(150, 88, 210, 220)
				: IM_COL32(95, 145, 220, 220);
			const ImU32 BorderCol = bIsSelected ? IM_COL32(255, 230, 110, 255) : IM_COL32(210, 185, 245, 220);

			DrawList->AddRectFilled(BarMin, BarMax, FillCol, 4.0f);
			DrawList->AddRect(BarMin, BarMax, BorderCol, 4.0f, 0, bIsSelected ? 2.0f : 1.0f);
			DrawList->AddRectFilled(ImVec2(EndX - MontageEndHandleWidth, BarMin.y), BarMax, IM_COL32(255, 255, 255, 70), 2.0f);
			DrawList->AddText(ImVec2(StartX + 5.0f, NotY + 10.0f), IM_COL32(250, 250, 255, 255), DisplayName.c_str());

			if (MousePos.x >= BarMin.x && MousePos.x <= BarMax.x + 3.0f && MousePos.y >= BarMin.y && MousePos.y <= BarMax.y)
			{
				HoveredNotifyIdx = Idx;
				bHoveredRightHandle = (MousePos.x >= EndX - MontageEndHandleWidth);
			}
		}
		else
		{
			const float CenterY = NotY + MontageNotifyRowHeight * 0.5f;
			const ImVec2 P0(StartX, CenterY - MontageDiamondRadius);
			const ImVec2 P1(StartX + MontageDiamondRadius, CenterY);
			const ImVec2 P2(StartX, CenterY + MontageDiamondRadius);
			const ImVec2 P3(StartX - MontageDiamondRadius, CenterY);
			const ImU32 FillCol = IM_COL32(235, 165, 65, 240);
			const ImU32 BorderCol = bIsSelected ? IM_COL32(255, 255, 255, 255) : IM_COL32(160, 100, 25, 255);

			DrawList->AddQuadFilled(P0, P1, P2, P3, FillCol);
			DrawList->AddQuad(P0, P1, P2, P3, BorderCol, bIsSelected ? 2.0f : 1.5f);
			DrawList->AddText(ImVec2(StartX + MontageDiamondRadius + 4.0f, CenterY - 7.0f), IM_COL32(245, 235, 210, 255), DisplayName.c_str());

			if (std::abs(MousePos.x - StartX) <= MontageDiamondRadius + 4.0f && MousePos.y >= NotY && MousePos.y <= NotY + MontageNotifyRowHeight)
			{
				HoveredNotifyIdx = Idx;
				bHoveredRightHandle = false;
			}
		}
	}

	// 재생 헤드 그리기
	const float PlayheadX = TrackOrigin.x + std::clamp(CurrentPos, 0.0f, TotalLength) * PixelsPerSec;
	DrawList->AddTriangleFilled(
		ImVec2(PlayheadX - 6.0f, CanvasOrigin.y + 2.0f),
		ImVec2(PlayheadX + 6.0f, CanvasOrigin.y + 2.0f),
		ImVec2(PlayheadX, CanvasOrigin.y + 12.0f),
		IM_COL32(235, 75, 75, 255));
	DrawList->AddLine(
		ImVec2(PlayheadX, CanvasOrigin.y + 12.0f),
		ImVec2(PlayheadX, CanvasOrigin.y + TotalRowsHeight),
		IM_COL32(235, 75, 75, 230), 2.0f);

	// 타임라인 마우스 상호작용 처리
	ImGui::SetCursorScreenPos(TrackOrigin);
	ImGui::InvisibleButton("##MontageTimelineHit", ImVec2(TimelineWidth, TotalRowsHeight),
		ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
	const bool bHovered = ImGui::IsItemHovered();

	if (bHovered && bHoveredRightHandle)
	{
		ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
	}

	if (bHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
	{
		if (MousePos.y >= NotY && HoveredNotifyIdx != INDEX_NONE)
		{
			Editor.SetSelectedNotifyIndex(HoveredNotifyIdx);
			DragNotifyIndex = HoveredNotifyIdx;
			DragStartMouseX = MousePos.x;
			if (bHoveredRightHandle && Asset.Notifies[HoveredNotifyIdx].Duration > 0.0f)
			{
				DragMode = ETimelineDragMode::ResizeNotifyEnd;
				DragStartValue = Asset.Notifies[HoveredNotifyIdx].Duration;
			}
			else
			{
				DragMode = ETimelineDragMode::MoveNotify;
				DragStartValue = Asset.Notifies[HoveredNotifyIdx].Time;
			}
		}
		else
		{
			DragMode = ETimelineDragMode::Scrub;
			const float ScrubTime = std::clamp((MousePos.x - TrackOrigin.x) / PixelsPerSec, 0.0f, TotalLength);
			Editor.PreviewScrub(ScrubTime);
		}
	}

	if (DragMode != ETimelineDragMode::None)
	{
		if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
		{
			const float DeltaSec = (MousePos.x - DragStartMouseX) / PixelsPerSec;
			if (DragMode == ETimelineDragMode::Scrub)
			{
				const float ScrubTime = std::clamp((MousePos.x - TrackOrigin.x) / PixelsPerSec, 0.0f, TotalLength);
				Editor.PreviewScrub(ScrubTime);
			}
			else if (DragMode == ETimelineDragMode::MoveNotify && DragNotifyIndex >= 0 && DragNotifyIndex < Asset.Notifies.Num())
			{
				Asset.Notifies[DragNotifyIndex].Time = std::clamp(DragStartValue + DeltaSec, 0.0f, TotalLength);
				Editor.RefreshPreviewMontage();
			}
			else if (DragMode == ETimelineDragMode::ResizeNotifyEnd && DragNotifyIndex >= 0 && DragNotifyIndex < Asset.Notifies.Num())
			{
				const float MaxDur = std::max(0.02f, TotalLength - Asset.Notifies[DragNotifyIndex].Time);
				Asset.Notifies[DragNotifyIndex].Duration = std::clamp(DragStartValue + DeltaSec, 0.02f, MaxDur);
				Editor.RefreshPreviewMontage();
			}
		}
		else
		{
			DragMode = ETimelineDragMode::None;
			DragNotifyIndex = INDEX_NONE;
		}
	}

	if (bHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && MousePos.y >= NotY)
	{
		ContextPopupTime = std::clamp((MousePos.x - TrackOrigin.x) / PixelsPerSec, 0.0f, TotalLength);
		ContextNotifyIndex = HoveredNotifyIdx;
		if (ContextNotifyIndex != INDEX_NONE)
			Editor.SetSelectedNotifyIndex(ContextNotifyIndex);
		ImGui::OpenPopup("MontageTimelineNotifyCtx");
	}

	if (ImGui::BeginPopup("MontageTimelineNotifyCtx"))
	{
		if (ImGui::MenuItem("Add Particle Effect (NotifyState)"))
		{
			FAnimMontageNotify Notify;
			Notify.Name = "ParticleEffect";
			Notify.ClassName = "UAnimNotifyState_ParticleEffect";
			Notify.Time = ContextPopupTime;
			Notify.Duration = std::min(0.25f, std::max(0.05f, TotalLength - ContextPopupTime));
			Asset.Notifies.Add(Notify);
			Editor.SetSelectedNotifyIndex(Asset.Notifies.Num() - 1);
			Editor.RefreshPreviewMontage();
		}
		if (ImGui::MenuItem("Add Named NotifyState"))
		{
			FAnimMontageNotify Notify;
			Notify.Name = "NotifyState";
			Notify.ClassName = "UAnimNotifyState_Named";
			Notify.Time = ContextPopupTime;
			Notify.Duration = std::min(0.25f, std::max(0.05f, TotalLength - ContextPopupTime));
			Asset.Notifies.Add(Notify);
			Editor.SetSelectedNotifyIndex(Asset.Notifies.Num() - 1);
			Editor.RefreshPreviewMontage();
		}
		if (ImGui::MenuItem("Add Notify"))
		{
			FAnimMontageNotify Notify;
			Notify.Name = "Notify";
			Notify.Time = ContextPopupTime;
			Asset.Notifies.Add(Notify);
			Editor.SetSelectedNotifyIndex(Asset.Notifies.Num() - 1);
			Editor.RefreshPreviewMontage();
		}
		if (ContextNotifyIndex >= 0 && ContextNotifyIndex < Asset.Notifies.Num())
		{
			ImGui::Separator();
			if (ImGui::MenuItem("Delete Notify"))
			{
				Asset.Notifies.RemoveAt(ContextNotifyIndex, 1);
				Editor.SetSelectedNotifyIndex(INDEX_NONE);
				ContextNotifyIndex = INDEX_NONE;
				Editor.RefreshPreviewMontage();
			}
		}
		ImGui::EndPopup();
	}

	ImGui::EndChild();
}

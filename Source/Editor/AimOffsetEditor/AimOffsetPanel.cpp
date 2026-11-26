#include "EnginePCH.h"
#include "AimOffsetPanel.h"
#include "Animation/AnimSequenceFile.h"
#include "Asset/AssetUtils.h"
#include "Core/Misc/Paths.h"
#include <algorithm>
#include <filesystem>

void FAimOffsetPanel::RefreshSequences()
{
	SequencePaths.clear();
	bInitialized = true;

	const FString& PreviewPath = Editor.GetAimOffsetAsset().PreviewMeshPath;
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

void FAimOffsetPanel::DrawSequenceCombo(const char* Label, FString& InOutPath)
{
	if (!bInitialized)
		RefreshSequences();

	const FString Display = InOutPath.empty() ? FString("None")
		: std::filesystem::path(InOutPath).filename().generic_string();
	if (ImGui::BeginCombo(Label, Display.c_str()))
	{
		if (ImGui::Selectable("None", InOutPath.empty()))
			InOutPath.clear();
		for (const FString& Path : SequencePaths)
		{
			const bool bSelected = (InOutPath == Path);
			if (ImGui::Selectable(Path.c_str(), bSelected))
				InOutPath = Path;
		}
		ImGui::EndCombo();
	}
}

void FAimOffsetPanel::DrawContent()
{
	if (ImGui::Button("Save"))
		Editor.Save();
	ImGui::SameLine();
	if (ImGui::Button("Refresh Sequences"))
		RefreshSequences();

	const FString& Message = Editor.GetMessage();
	if (!Message.empty())
		ImGui::TextWrapped("%s", Message.c_str());

	FAimOffsetAsset& Asset = Editor.GetAimOffsetAsset();
	ImGui::SeparatorText("Preview");
	if (ImGui::SliderFloat("Pitch", &Editor.PreviewPitch, Asset.MinPitch, Asset.MaxPitch))
		Editor.SyncPreviewParameters();
	if (ImGui::SliderFloat("Alpha", &Editor.PreviewAlpha, 0.0f, 1.0f))
		Editor.SyncPreviewParameters();

	ImGui::SeparatorText("Base Pose");
	DrawSequenceCombo("Base Sequence", Asset.BaseSequencePath);
	ImGui::InputInt("Base Frame Index", &Asset.BaseFrameIndex);
	ImGui::DragFloat("Min Pitch", &Asset.MinPitch, 1.0f, -180.0f, 0.0f);
	ImGui::DragFloat("Max Pitch", &Asset.MaxPitch, 1.0f, 0.0f, 180.0f);

	ImGui::SeparatorText("Samples");
	if (ImGui::Button("Add Sample"))
	{
		FAimOffsetSample Sample;
		Sample.Pitch = 0.0f;
		Sample.SequencePath = Asset.BaseSequencePath;
		Asset.Samples.Add(Sample);
	}

	int32 RemoveIdx = -1;
	for (int32 Idx = 0; Idx < Asset.Samples.Num(); ++Idx)
	{
		ImGui::PushID(Idx);
		FAimOffsetSample& Sample = Asset.Samples[Idx];
		ImGui::DragFloat("Sample Pitch", &Sample.Pitch, 1.0f, Asset.MinPitch, Asset.MaxPitch);
		DrawSequenceCombo("Sample Sequence", Sample.SequencePath);
		ImGui::InputInt("Frame Index", &Sample.FrameIndex);
		if (ImGui::SmallButton("Remove Sample"))
			RemoveIdx = Idx;
		ImGui::Separator();
		ImGui::PopID();
	}
	if (RemoveIdx >= 0)
		Asset.Samples.RemoveAt(RemoveIdx, 1);
}

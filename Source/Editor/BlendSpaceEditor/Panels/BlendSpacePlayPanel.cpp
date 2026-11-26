#include "EnginePCH.h"
#include "BlendSpacePlayPanel.h"

#include <cmath>
#include <cstdio>
#include <algorithm>
#include <filesystem>
#include "Asset/AssetUtils.h"
#include "Core/Misc/Paths.h"

#include "Editor/BlendSpaceEditor/BlendSpaceEditor.h"
#include "Components/SkeletalMeshComponent.h"

#include "Animation/AnimSingleNodeInstance.h"
#include "Animation/BlendSpace1D.h"
#include "Animation/BlendSpace2D.h"
#include "Animation/AnimSequenceFile.h"

#include "Asset/AssetManager.h"
#include "Render/SkeletalMesh.h"

void FBlendSpacePlayPanel::DrawContent()
{
    if (ImGui::Button("Save"))
        Editor.Save();

    const FString& Message = Editor.GetMessage();
    if (!Message.empty())
        ImGui::TextWrapped("%s", Message.c_str());

    DrawSamples();
    ImGui::SeparatorText("Playback");

    USkeletalMeshComponent* Component = Editor.GetMeshComponent();
    if (!Component)
    {
        ImGui::TextUnformatted("No preview mesh.");
        return;
    }

    UAnimSingleNodeInstance* Instance = Component->GetSingleNodeInstance();
    if (!Instance)
    {
        ImGui::TextUnformatted("No animation instance.");
        return;
    }

    if (ImGui::Checkbox("Loop", &bLoop))
        Instance->SetLooping(bLoop);

    if (ImGui::Button("Play / Resume"))
        Component->Play(bLoop);
    ImGui::SameLine();
    if (ImGui::Button("Stop"))
        Component->Stop();

    float Rate = Instance->GetPlayRate();
    if (ImGui::SliderFloat("Play rate", &Rate, -2.0f, 2.0f))
        Instance->SetPlayRate(Rate);
    if (ImGui::DragFloat("Blend input X", &Component->BlendSpaceInput))
        Component->RefreshAnimationPose();
    if (Editor.Is2D() && ImGui::DragFloat("Blend input Y", &Component->BlendSpaceInputY))
        Component->RefreshAnimationPose();

    const float Length = Instance->GetLength();
    float Time = Instance->GetCurrentTime();
    if (Length > 0.0f && ImGui::SliderFloat("Time", &Time, 0.0f, Length))
    {
        Component->Stop();
        Instance->SetPosition(Time);
        Component->RefreshAnimationPose();
    }

    ImGui::Text("%s | %.3f / %.3f seconds",
        Component->IsPlaying() ? "Playing" : "Stopped",
        Instance->GetCurrentTime(), Instance->GetLength());

    ImGui::SeparatorText("Camera");
    float CameraSpeed = Editor.GetCameraSpeed();
    if (ImGui::DragFloat("Camera speed", &CameraSpeed, 0.5f, 1.0f, 150.0f, "%.1f"))
    {
        Editor.SetCameraSpeed(CameraSpeed);
    }
}

void FBlendSpacePlayPanel::DrawSamples()
{
    UBlendSpace1D* BlendSpace = Editor.GetBlendSpace();
    UBlendSpace2D* BlendSpace2D = Editor.GetBlendSpace2D();
    USkeletalMeshComponent* Component = Editor.GetMeshComponent();
    if ((!BlendSpace && !BlendSpace2D) || !Component)
    {
        return;
    }

    ImGui::SeparatorText(BlendSpace2D ? "Samples (2D)" : "Samples (1D)");
    if (!bAnimationListInitialized)
    {
        RefreshAnimationList();
    }
    const char* Selected = SelectedAnimation >= 0 ? AnimationPaths[SelectedAnimation].c_str() : "Select animation";
    
    if (ImGui::BeginCombo("Animation", Selected))
    {
        for (int Index = 0; Index < static_cast<int>(AnimationPaths.size()); ++Index)
        {
            if (ImGui::Selectable(AnimationPaths[Index].c_str(), SelectedAnimation == Index))
                SelectedAnimation = Index;
        }
        ImGui::EndCombo();
    }

    if (AnimationPaths.empty())
    {
        ImGui::TextUnformatted("No .anim.bin files in this BlendSpace folder.");
    }

    ImGui::InputFloat("Sample X", &SampleX);
    if (BlendSpace2D)
    {
        ImGui::InputFloat("Sample Y", &SampleY);
    }

    ImGui::BeginDisabled(SelectedAnimation < 0);
    if (ImGui::Button("Add Sample"))
    {
        SampleMessage.clear();
        UAnimSequence* Sequence = nullptr;

        if (std::isfinite(SampleX) && (!BlendSpace2D || std::isfinite(SampleY)))
        {
            Sequence = UAssetManager::LoadAnimSequence(AnimationPaths[SelectedAnimation]);
        }
        else
        {
            SampleMessage = "Sample coordinate must be finite.";
        }
        
        if (!Sequence || !Sequence->GetDataModel())
        {
            if (SampleMessage.empty())
                SampleMessage = "Failed to load animation.";
        }

        if (Sequence && Sequence->GetDataModel())
        {
            bool bCompatible = true;
            for (const FBoneAnimationTrack& Track : Sequence->GetDataModel()->GetBoneAnimationTracks())
            {
                if (Component->FindBoneIndex(Track.Name) < 0)
                {
                    bCompatible = false;
                    break;
                }
            }

            if (!bCompatible)
            {
                SampleMessage = "Animation bones do not match the preview mesh.";
            }
            else
            {
                const bool bAdded = BlendSpace2D
                    ? BlendSpace2D->AddSample(Sequence, FVector(SampleX, SampleY, 0.0f))
                    : BlendSpace->AddSample(Sequence, FVector(SampleX, 0.0f, 0.0f));
                if (!bAdded)
                {
                    SampleMessage = "A sample already exists at this position.";
                }
                else
                {
                    Component->RefreshAnimationPose();
                    SampleMessage = "Sample added. Press Save to keep changes.";
                }
            }
        }
    }

    ImGui::EndDisabled();

    if (!SampleMessage.empty())
        ImGui::TextWrapped("%s", SampleMessage.c_str());

	const TArray<FBlendSample>& Samples = BlendSpace2D ? BlendSpace2D->GetSampleData() : BlendSpace->GetSampleData();
	ImGui::Text("Samples: %d", Samples.Num());

	int32 RemoveIndex = -1;
    for (int32 Index = 0; Index < Samples.Num(); ++Index)
    {
        const FBlendSample& Sample = Samples[Index];
        ImGui::PushID(Index);

        if (ImGui::SmallButton("Remove"))
        {
            RemoveIndex = Index;
        }

        ImGui::SameLine();
        if (BlendSpace2D)
            ImGui::Text("X: %.2f, Y: %.2f", Sample.SampleValue.X, Sample.SampleValue.Y);
        else
            ImGui::Text("X: %.2f", Sample.SampleValue.X);

        const char* Path = Sample.Animation ? Sample.Animation->GetAssetPath().c_str() : "(missing)";
        ImGui::TextWrapped("%s", Path);
        ImGui::PopID();
    }

    if (RemoveIndex >= 0)
    {
        if (BlendSpace2D)
            BlendSpace2D->RemoveSample(RemoveIndex);
        else
            BlendSpace->RemoveSample(RemoveIndex);
        Component->RefreshAnimationPose();
    }
}

void FBlendSpacePlayPanel::RefreshAnimationList()
{
    FString Previous;
    if (SelectedAnimation >= 0 && SelectedAnimation < static_cast<int>(AnimationPaths.size()))
    {
        Previous = AnimationPaths[SelectedAnimation];
    }

    AnimationPaths.clear();
    SelectedAnimation = -1;
    bAnimationListInitialized = true;
    SampleMessage.clear();

    FString FilePath = FAssetUtils::ResolveAssetFilePath(Editor.GetAssetPath());
    if (FilePath.empty())
    {
        FilePath = Editor.GetAssetPath();
    }
    std::filesystem::path BlendSpacePath(FilePath);
    std::filesystem::path Directory = BlendSpacePath.parent_path();

    std::error_code Error;
    std::filesystem::directory_iterator It(Directory, Error);
    std::filesystem::directory_iterator End;

	while (!Error && It != End)
	{
		bool bIsFile = It->is_regular_file(Error);
		if (bIsFile)
		{
			FString Path = It->path().generic_string();
			if (AnimSequenceFile::IsAnimSequencePath(Path))
			{
				FString AssetPath = FAssetUtils::GetAssetKey(Path);
				AnimationPaths.push_back(AssetPath);
			}
		}
		if (!Error)
		{
			It.increment(Error);
		}
	}

	if (AnimationPaths.empty())
	{
		// 현재 폴더에 시퀀스가 없으면 콘텐츠 전체 폴더 검색
		std::error_code ContentErr;
		const std::filesystem::path ContentDir = FPaths::ProjectContentDir();
		for (const std::filesystem::directory_entry& Entry : std::filesystem::recursive_directory_iterator(ContentDir, ContentErr))
		{
			if (!Entry.is_regular_file(ContentErr))
				continue;
			const FString Path = Entry.path().generic_string();
			if (AnimSequenceFile::IsAnimSequencePath(Path))
			{
				AnimationPaths.push_back(FAssetUtils::GetAssetKey(Path));
			}
		}
	}

	std::sort(AnimationPaths.begin(), AnimationPaths.end());
	if (Error && AnimationPaths.empty())
	{
		SampleMessage = "Cannot read animation folder: " + Error.message();
	}
}

void FBlendSpaceGridPanel::DrawContent()
{
	UBlendSpace1D* BlendSpace = Editor.GetBlendSpace();
	UBlendSpace2D* BlendSpace2D = Editor.GetBlendSpace2D();
	USkeletalMeshComponent* Component = Editor.GetMeshComponent();
	if ((!BlendSpace && !BlendSpace2D) || !Component)
	{
		ImGui::TextUnformatted("No BlendSpace asset loaded.");
		return;
	}

	const TArray<FBlendSample>& Samples = BlendSpace2D ? BlendSpace2D->GetSampleData() : BlendSpace->GetSampleData();
	float MinX = -180.0f;
	float MaxX = 180.0f;
	float MinY = 0.0f;
	float MaxY = 600.0f;

	if (!Samples.IsEmpty())
	{
		MinX = Samples[0].SampleValue.X;
		MaxX = MinX;
		MinY = Samples[0].SampleValue.Y;
		MaxY = MinY;
		for (const FBlendSample& Sample : Samples)
		{
			MinX = std::min(MinX, Sample.SampleValue.X);
			MaxX = std::max(MaxX, Sample.SampleValue.X);
			MinY = std::min(MinY, Sample.SampleValue.Y);
			MaxY = std::max(MaxY, Sample.SampleValue.Y);
		}
		if (MaxX - MinX < 1.0f)
		{
			MinX -= 90.0f;
			MaxX += 90.0f;
		}
		if (MaxY - MinY < 1.0f)
		{
			MinY -= 100.0f;
			MaxY += 100.0f;
		}
	}

	const float SpanX = std::max(1.0f, MaxX - MinX);
	const float SpanY = std::max(1.0f, MaxY - MinY);

	if (BlendSpace2D)
	{
		ImGui::Text("Hold Ctrl + move mouse (or drag) to move preview point  |  X: %.1f, Y: %.1f",
			Component->BlendSpaceInput, Component->BlendSpaceInputY);
	}
	else
	{
		ImGui::Text("Hold Ctrl + move mouse (or drag) to move preview point  |  X: %.1f",
			Component->BlendSpaceInput);
	}
	ImGui::SameLine();
	if (ImGui::SmallButton("Reset Preview"))
	{
		Component->BlendSpaceInput = std::clamp(0.0f, MinX, MaxX);
		if (BlendSpace2D)
			Component->BlendSpaceInputY = std::clamp(0.0f, MinY, MaxY);
		Component->RefreshAnimationPose();
	}

	const ImVec2 Avail = ImGui::GetContentRegionAvail();
	const ImVec2 CanvasSize(std::max(160.0f, Avail.x), std::max(130.0f, Avail.y));
	const ImVec2 CanvasOrigin = ImGui::GetCursorScreenPos();
	const ImVec2 CanvasEnd(CanvasOrigin.x + CanvasSize.x, CanvasOrigin.y + CanvasSize.y);

	const float LeftPad = 56.0f;
	const float RightPad = 24.0f;
	const float TopPad = 18.0f;
	const float BottomPad = 28.0f;
	const ImVec2 PlotMin(CanvasOrigin.x + LeftPad, CanvasOrigin.y + TopPad);
	const ImVec2 PlotMax(std::max(PlotMin.x + 20.0f, CanvasEnd.x - RightPad),
		std::max(PlotMin.y + 20.0f, CanvasEnd.y - BottomPad));
	const float PlotW = PlotMax.x - PlotMin.x;
	const float PlotH = PlotMax.y - PlotMin.y;

	ImDrawList* DrawList = ImGui::GetWindowDrawList();
	DrawList->AddRectFilled(CanvasOrigin, CanvasEnd, IM_COL32(22, 24, 29, 255));
	DrawList->AddRect(CanvasOrigin, CanvasEnd, IM_COL32(70, 76, 88, 255));
	DrawList->AddRectFilled(PlotMin, PlotMax, IM_COL32(30, 33, 40, 255));

	const int32 GridCols = 8;
	const int32 GridRows = BlendSpace2D ? 4 : 2;
	for (int32 Col = 0; Col <= GridCols; ++Col)
	{
		const float Ratio = static_cast<float>(Col) / static_cast<float>(GridCols);
		const float X = PlotMin.x + Ratio * PlotW;
		DrawList->AddLine(ImVec2(X, PlotMin.y), ImVec2(X, PlotMax.y), IM_COL32(58, 62, 72, 200));

		char ValBuf[32] = {};
		std::snprintf(ValBuf, sizeof(ValBuf), "%.0f", MinX + Ratio * SpanX);
		DrawList->AddText(ImVec2(X - 12.0f, PlotMax.y + 5.0f), IM_COL32(175, 182, 195, 255), ValBuf);
	}

	if (BlendSpace2D)
	{
		for (int32 Row = 0; Row <= GridRows; ++Row)
		{
			const float Ratio = static_cast<float>(Row) / static_cast<float>(GridRows);
			const float Y = PlotMax.y - Ratio * PlotH;
			DrawList->AddLine(ImVec2(PlotMin.x, Y), ImVec2(PlotMax.x, Y), IM_COL32(58, 62, 72, 200));

			char ValBuf[32] = {};
			std::snprintf(ValBuf, sizeof(ValBuf), "%.0f", MinY + Ratio * SpanY);
			DrawList->AddText(ImVec2(CanvasOrigin.x + 8.0f, Y - 7.0f), IM_COL32(175, 182, 195, 255), ValBuf);
		}
	}
	else
	{
		const float MidY = (PlotMin.y + PlotMax.y) * 0.5f;
		DrawList->AddLine(ImVec2(PlotMin.x, MidY), ImVec2(PlotMax.x, MidY), IM_COL32(95, 105, 125, 220), 1.5f);
	}

	// 기준 원점 축 강조
	if (MinX < 0.0f && MaxX > 0.0f)
	{
		const float ZeroX = PlotMin.x + ((0.0f - MinX) / SpanX) * PlotW;
		DrawList->AddLine(ImVec2(ZeroX, PlotMin.y), ImVec2(ZeroX, PlotMax.y), IM_COL32(95, 105, 125, 180), 1.5f);
	}
	if (BlendSpace2D && MinY < 0.0f && MaxY > 0.0f)
	{
		const float ZeroY = PlotMax.y - ((0.0f - MinY) / SpanY) * PlotH;
		DrawList->AddLine(ImVec2(PlotMin.x, ZeroY), ImVec2(PlotMax.x, ZeroY), IM_COL32(95, 105, 125, 180), 1.5f);
	}

	DrawList->AddRect(PlotMin, PlotMax, IM_COL32(110, 118, 134, 255));

	// 삼각화 메시 연결선
	if (BlendSpace2D)
	{
		for (const FBlendTriangle& Tri : BlendSpace2D->GetTriangles())
		{
			if (Tri.A < 0 || Tri.A >= Samples.Num() || Tri.B < 0 || Tri.B >= Samples.Num() || Tri.C < 0 || Tri.C >= Samples.Num())
				continue;
			const ImVec2 PA(
				PlotMin.x + ((Samples[Tri.A].SampleValue.X - MinX) / SpanX) * PlotW,
				PlotMax.y - ((Samples[Tri.A].SampleValue.Y - MinY) / SpanY) * PlotH);
			const ImVec2 PB(
				PlotMin.x + ((Samples[Tri.B].SampleValue.X - MinX) / SpanX) * PlotW,
				PlotMax.y - ((Samples[Tri.B].SampleValue.Y - MinY) / SpanY) * PlotH);
			const ImVec2 PC(
				PlotMin.x + ((Samples[Tri.C].SampleValue.X - MinX) / SpanX) * PlotW,
				PlotMax.y - ((Samples[Tri.C].SampleValue.Y - MinY) / SpanY) * PlotH);
			DrawList->AddTriangle(PA, PB, PC, IM_COL32(125, 165, 220, 160), 1.0f);
		}
	}

	// 마우스 상호작용 처리
	ImGui::SetCursorScreenPos(PlotMin);
	ImGui::InvisibleButton("##BlendSpaceGridCanvas", ImVec2(PlotW, PlotH), ImGuiButtonFlags_MouseButtonLeft);
	const bool bCanvasHovered = ImGui::IsItemHovered();
	const ImVec2 MousePos = ImGui::GetMousePos();

	if (bCanvasHovered && (ImGui::GetIO().KeyCtrl || ImGui::IsMouseDown(ImGuiMouseButton_Left)))
	{
		const float NormX = std::clamp((MousePos.x - PlotMin.x) / PlotW, 0.0f, 1.0f);
		const float NewX = MinX + NormX * SpanX;
		Component->BlendSpaceInput = NewX;
		if (BlendSpace2D)
		{
			const float NormY = std::clamp((PlotMax.y - MousePos.y) / PlotH, 0.0f, 1.0f);
			Component->BlendSpaceInputY = MinY + NormY * SpanY;
		}
		Component->RefreshAnimationPose();
	}

	// 샘플 포인트 및 호버 툴팁
	int32 HoveredSample = INDEX_NONE;
	for (int32 Index = 0; Index < Samples.Num(); ++Index)
	{
		const FBlendSample& Sample = Samples[Index];
		const float PosX = PlotMin.x + ((Sample.SampleValue.X - MinX) / SpanX) * PlotW;
		const float PosY = BlendSpace2D
			? (PlotMax.y - ((Sample.SampleValue.Y - MinY) / SpanY) * PlotH)
			: ((PlotMin.y + PlotMax.y) * 0.5f);
		const ImVec2 Pos(PosX, PosY);

		const float Dx = MousePos.x - Pos.x;
		const float Dy = MousePos.y - Pos.y;
		const bool bDotHovered = bCanvasHovered && (Dx * Dx + Dy * Dy <= 64.0f);
		if (bDotHovered)
			HoveredSample = Index;

		DrawList->AddCircleFilled(Pos, bDotHovered ? 6.5f : 5.0f, IM_COL32(115, 220, 155, 255));
		DrawList->AddCircle(Pos, bDotHovered ? 6.5f : 5.0f, IM_COL32(25, 35, 30, 255), 0, 1.5f);
	}

	// 현재 블렌드 프리뷰 커서
	const float ClampedX = std::clamp(Component->BlendSpaceInput, MinX, MaxX);
	const float CursorX = PlotMin.x + ((ClampedX - MinX) / SpanX) * PlotW;
	const float CursorY = BlendSpace2D
		? (PlotMax.y - ((std::clamp(Component->BlendSpaceInputY, MinY, MaxY) - MinY) / SpanY) * PlotH)
		: ((PlotMin.y + PlotMax.y) * 0.5f);

	DrawList->AddLine(ImVec2(CursorX, PlotMin.y), ImVec2(CursorX, PlotMax.y), IM_COL32(255, 190, 60, 150), 1.0f);
	if (BlendSpace2D)
	{
		DrawList->AddLine(ImVec2(PlotMin.x, CursorY), ImVec2(PlotMax.x, CursorY), IM_COL32(255, 190, 60, 150), 1.0f);
	}
	DrawList->AddCircleFilled(ImVec2(CursorX, CursorY), 6.5f, IM_COL32(255, 190, 60, 255));
	DrawList->AddCircle(ImVec2(CursorX, CursorY), 6.5f, IM_COL32(40, 25, 10, 255), 0, 1.5f);

	if (HoveredSample != INDEX_NONE && !ImGui::GetIO().KeyCtrl)
	{
		const FBlendSample& Sample = Samples[HoveredSample];
		const FString SeqName = Sample.Animation
			? std::filesystem::path(Sample.Animation->GetAssetPath()).filename().generic_string()
			: FString("(missing)");
		if (BlendSpace2D)
			ImGui::SetTooltip("%s\nX: %.1f, Y: %.1f", SeqName.c_str(), Sample.SampleValue.X, Sample.SampleValue.Y);
		else
			ImGui::SetTooltip("%s\nX: %.1f", SeqName.c_str(), Sample.SampleValue.X);
	}
}
#pragma once

#include "Core/StatOverlay.h"
#include "Core/EngineString.h"
#include "Physics/PhysScene.h"
#include "Editor/EditorUI/EditorStyle.h"

#include <algorithm>
#include <format>

// 레벨 뷰포트와 Physics Asset 프리뷰가 동일한 통계/단위를 사용한다.
namespace PhysicsStatOverlay
{
	inline TArray<FString> BuildLines(const FPhysScene* Scene)
	{
		TArray<FString> Lines;
		if (!Scene)
		{
			Lines.Add("  No physics scene");
			return Lines;
		}

		const FPhysicsSceneStats& Stats = Scene->GetStats();
		if (!Stats.bHasSimulatedFrame)
		{
			Lines.Add("  No simulation sample - start simulation");
			return Lines;
		}

		Lines.Add("  Last simulated frame / counts: last substep");
		Lines.Add(std::format("  Physics Time (calls + wait) {:.3f} ms", Stats.GetTotalCallsMs()));
		Lines.Add(std::format("    Simulate     {:.3f} ms", Stats.SimulateCallsMs));
		Lines.Add(std::format("    Fetch / Wait {:.3f} ms", Stats.FetchResultsMs));
		Lines.Add(std::format("    Sync         {:.3f} ms", Stats.SyncComponentsMs));
		Lines.Add(std::format("    Events       {:.3f} ms", Stats.DispatchEventsMs));
		Lines.Add(std::format("  Bodies  Static {} / Dynamic {} / Kinematic {}",
			Stats.StaticBodies, Stats.DynamicBodies, Stats.KinematicBodies));
		Lines.Add(std::format("  Active  Dynamic {} / Kinematic {}", Stats.ActiveDynamicBodies, Stats.ActiveKinematicBodies));
		Lines.Add(std::format("  Shapes {}   Active Constraints {}", Stats.Shapes, Stats.ActiveConstraints));
		Lines.Add(std::format("  Contact Pairs (non-CCD) {}", Stats.ContactPairs));
		Lines.Add(std::format("  Substeps {}   Step Delta {:.3f} ms", Stats.Substeps, Stats.StepDeltaTimeMs));
		return Lines;
	}

	inline void DrawPreview(ImDrawList* DrawList, const FPhysScene* Scene, const ImVec2& ViewMin, const ImVec2& ViewMax)
	{
		if (!DrawList || !FStatOverlay::IsEnabled(EStatFlags::Physics)) return;

		const TArray<FString> Lines = BuildLines(Scene);
		const float Margin = EditorStyle::Scale(8.0f);
		const float Padding = EditorStyle::Scale(6.0f);
		// 좌상단 기즈모 툴바 바로 아래에 배치한다.
		const ImVec2 Origin{ ViewMin.x + Margin, ViewMin.y + ImGui::GetFrameHeight() + Margin * 3.0f };
		const float LineHeight = ImGui::GetTextLineHeight();
		float Width = ImGui::CalcTextSize("Physics").x;
		for (const FString& Line : Lines)
			Width = std::max(Width, ImGui::CalcTextSize(Line.c_str()).x);

		DrawList->PushClipRect(ViewMin, ViewMax, true);
		DrawList->AddRectFilled({ Origin.x - Padding, Origin.y - Padding },
			{ Origin.x + Width + Padding, Origin.y + LineHeight * (Lines.Num() + 1) + Padding }, EditorColor::Scrim, 4.0f);
		DrawList->AddText(Origin, EditorColor::AccentText, "Physics");
		for (int32 Index = 0; Index < Lines.Num(); ++Index)
			DrawList->AddText({ Origin.x, Origin.y + LineHeight * (Index + 1) }, EditorColor::TextPrimary, Lines[Index].c_str());
		DrawList->PopClipRect();
	}
}

#include "EnginePCH.h"
#include "PhysicsAssetGraphPanel.h"
#include "Editor/PhysicsAssetEditor/PhysicsAssetEditor.h"
#include "Editor/EditorUI/EditorStyle.h"
#include "Physics/PhysicsAsset.h"
#include "Physics/BodySetup.h"
#include "Render/SkeletalMesh.h"

#include <cmath>
#include <cfloat>

FPhysicsAssetGraphPanel::FPhysicsAssetGraphPanel(FPhysicsAssetEditor& InEditor)
	: FPhysicsAssetEditorPanel(InEditor, "Constraint Graph")
{
}

void FPhysicsAssetGraphPanel::DrawContent()
{
	int32 ChildBody = Editor.GetSelectedBodyIndex();
	int32 SelectedConstraint = Editor.GetSelectedConstraintIndex();
	const int32 ParentBody = Editor.GetNearestParentBody(ChildBody);
	if (ImGui::Button("Fit Graph")) bFitRequested = true;
	ImGui::SameLine();
	if (ImGui::Checkbox("Show All", &bShowAllBodies)) bFitRequested = true;
	ImGui::SameLine();
	ImGui::BeginDisabled(!Editor.CanAddConstraint(ParentBody, ChildBody));
	if (ImGui::Button("Connect to Parent")) Editor.AddConstraint(ParentBody, ChildBody);
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::BeginDisabled(!Editor.GetSelectedConstraint() || Editor.IsSimulationActive());
	if (ImGui::Button("Delete Constraint")) Editor.RemoveConstraint(Editor.GetSelectedConstraintIndex());
	ImGui::EndDisabled();
	// 생성/삭제 버튼이 바꾼 선택도 같은 프레임의 필터에 반영한다.
	ChildBody = Editor.GetSelectedBodyIndex();
	SelectedConstraint = Editor.GetSelectedConstraintIndex();
	ImGui::TextWrapped("Select a body to view its direct connections, or a constraint to view its two bodies. Show All: overview. Middle/right drag: pan. Wheel: zoom.");

	const TArray<FPhysicsAssetPreviewBody>& Bodies = Editor.GetPreviewBodies();
	const UPhysicsAsset* Asset = Editor.GetPhysicsAsset();
	const USkeletalMesh* Mesh = Editor.GetMesh();
	if (!Asset || !Mesh || Bodies.IsEmpty())
	{
		ImGui::TextDisabled("Add physics bodies in the skeleton tree to build a constraint graph.");
		return;
	}

	// 본 계층의 바디 열 사이에 컨스트레인 전용 열을 확보합니다.
	const float Scale = EditorStyle::Scale(1.0f);
	const ImVec2 NodeSize(200.0f * Scale, 52.0f * Scale);
	const ImVec2 ConstraintNodeSize(220.0f * Scale, 64.0f * Scale);
	const float Gap = 48.0f * Scale;
	const float ColumnStride = NodeSize.x + ConstraintNodeSize.x + Gap * 2.0f;
	const float RowStride = 100.0f * Scale;
	TArray<int32> ConstraintParents, ConstraintChildren, IncomingCounts, IncomingSlots;
	ConstraintParents.Init(-1, Asset->Constraints.Num());
	ConstraintChildren.Init(-1, Asset->Constraints.Num());
	IncomingCounts.Init(0, Bodies.Num());
	IncomingSlots.Init(0, Bodies.Num());
	for (int32 Index = 0; Index < Asset->Constraints.Num(); ++Index)
	{
		const FPhysicsAssetConstraint& Constraint = Asset->Constraints[Index];
		const int32 Parent = Editor.FindBodyByBone(Mesh->FindBoneIndex(Constraint.ParentBone));
		const int32 Child = Editor.FindBodyByBone(Mesh->FindBoneIndex(Constraint.ChildBone));
		if (Parent < 0 || Child < 0) continue;
		ConstraintParents[Index] = Parent;
		ConstraintChildren[Index] = Child;
	}
	const bool bBodySelected = ChildBody >= 0 && ChildBody < Bodies.Num();
	const bool bConstraintSelected = SelectedConstraint >= 0 && SelectedConstraint < Asset->Constraints.Num()
		&& ConstraintParents[SelectedConstraint] >= 0 && ConstraintChildren[SelectedConstraint] >= 0;
	const bool bFocusSelection = !bShowAllBodies && (bBodySelected || bConstraintSelected);
	TArray<uint8> VisibleBodies, VisibleConstraints;
	VisibleBodies.Init(bFocusSelection ? 0 : 1, Bodies.Num());
	VisibleConstraints.Init(0, Asset->Constraints.Num());
	if (bFocusSelection && bBodySelected) VisibleBodies[ChildBody] = 1;
	int32 NumVisibleConstraints = 0;
	for (int32 Index = 0; Index < Asset->Constraints.Num(); ++Index)
	{
		const int32 Parent = ConstraintParents[Index], Child = ConstraintChildren[Index];
		if (Parent < 0 || Child < 0) continue;
		// 전체 연결 성분을 순회하지 않고 선택한 바디에 직접 붙은 제약만 포함한다.
		if (bFocusSelection && !(bBodySelected ? Parent == ChildBody || Child == ChildBody : Index == SelectedConstraint)) continue;
		VisibleConstraints[Index] = 1;
		VisibleBodies[Parent] = VisibleBodies[Child] = 1;
		++IncomingCounts[Child];
		++NumVisibleConstraints;
	}
	int32 NumVisibleBodies = 0;
	for (uint8 bVisible : VisibleBodies) NumVisibleBodies += bVisible;
	ImGui::TextDisabled("Bodies: %d / %d  Constraints: %d / %d%s", NumVisibleBodies, Bodies.Num(),
		NumVisibleConstraints, Asset->Constraints.Num(), !bShowAllBodies && !bFocusSelection ? "  (select a body to focus)" : "");
	const ImVec2 Size = ImGui::GetContentRegionAvail();
	if (Size.x < 1.0f || Size.y < 1.0f) return;
	TArray<ImVec2> Positions;
	Positions.SetNum(Bodies.Num());
	TArray<ImVec2> ConstraintPositions;
	ConstraintPositions.SetNum(Asset->Constraints.Num());
	TArray<int32> Rows;
	Rows.Init(0, Bodies.Num() + 1);
	ImVec2 Bounds(-FLT_MAX, -FLT_MAX);
	ImVec2 BoundsMin(FLT_MAX, FLT_MAX);
	const auto IncludeNode = [&](const ImVec2& Position, const ImVec2& NodeExtent)
	{
		BoundsMin.x = FMath::Min(BoundsMin.x, Position.x);
		BoundsMin.y = FMath::Min(BoundsMin.y, Position.y);
		Bounds.x = FMath::Max(Bounds.x, Position.x + NodeExtent.x);
		Bounds.y = FMath::Max(Bounds.y, Position.y + NodeExtent.y);
	};
	for (int32 Index = 0; Index < Bodies.Num(); ++Index)
	{
		if (!VisibleBodies[Index]) continue;
		int32 Depth = 0;
		if (bFocusSelection)
		{
			// 선택 바디를 가운데 두고 실제 제약의 부모/자식 바디를 좌우에 배치한다.
			Depth = bBodySelected ? 1 : (Index == ConstraintParents[SelectedConstraint] ? 0 : 1);
			if (bBodySelected && Index != ChildBody)
			{
				for (int32 ConstraintIndex = 0; ConstraintIndex < Asset->Constraints.Num(); ++ConstraintIndex)
				{
					if (!VisibleConstraints[ConstraintIndex]) continue;
					if (ConstraintParents[ConstraintIndex] == Index && ConstraintChildren[ConstraintIndex] == ChildBody) Depth = 0;
					else if (ConstraintParents[ConstraintIndex] == ChildBody && ConstraintChildren[ConstraintIndex] == Index) Depth = 2;
				}
			}
		}
		else
		{
			for (int32 Parent = Editor.GetNearestParentBody(Index); Parent >= 0 && Depth < Bodies.Num();
				Parent = Editor.GetNearestParentBody(Parent)) ++Depth;
		}
		const int32 SlotCount = FMath::Max(1, IncomingCounts[Index]);
		Positions[Index] = ImVec2(Depth * ColumnStride,
			(Rows[Depth] + (SlotCount - 1) * 0.5f) * RowStride + (ConstraintNodeSize.y - NodeSize.y) * 0.5f);
		Rows[Depth] += SlotCount;
		IncludeNode(Positions[Index], NodeSize);
	}
	for (int32 Index = 0; Index < Asset->Constraints.Num(); ++Index)
	{
		if (!VisibleConstraints[Index]) continue;
		const int32 Parent = ConstraintParents[Index], Child = ConstraintChildren[Index];
		if (Parent < 0 || Child < 0) continue;
		// 실제 에셋의 제약마다 노드를 만들고, 같은 자식에 제약이 여러 개여도 겹치지 않게 배치합니다.
		const float SlotOffset = IncomingSlots[Child]++ - (IncomingCounts[Child] - 1) * 0.5f;
		ConstraintPositions[Index] = ImVec2(Positions[Child].x - Gap - ConstraintNodeSize.x,
			Positions[Child].y + (NodeSize.y - ConstraintNodeSize.y) * 0.5f + SlotOffset * RowStride);
		IncludeNode(ConstraintPositions[Index], ConstraintNodeSize);
	}
	for (ImVec2& Position : Positions) { Position.x -= BoundsMin.x; Position.y -= BoundsMin.y; }
	for (ImVec2& Position : ConstraintPositions) { Position.x -= BoundsMin.x; Position.y -= BoundsMin.y; }
	Bounds.x -= BoundsMin.x;
	Bounds.y -= BoundsMin.y;
	if (!bShowAllBodies && (LastSelectedBody != ChildBody || LastSelectedConstraint != SelectedConstraint)) bFitRequested = true;
	LastSelectedBody = ChildBody;
	LastSelectedConstraint = SelectedConstraint;
	if (LastTopologyRevision != Editor.GetTopologyRevision())
	{
		LastTopologyRevision = Editor.GetTopologyRevision();
		bFitRequested = true;
	}
	if (bFitRequested)
	{
		Zoom = FMath::Clamp(FMath::Min(Size.x * 0.9f / Bounds.x, Size.y * 0.9f / Bounds.y), 0.2f, 1.5f);
		Pan = ImVec2((Size.x - Bounds.x * Zoom) * 0.5f, (Size.y - Bounds.y * Zoom) * 0.5f);
		bFitRequested = false;
	}

	const ImVec2 Origin = ImGui::GetCursorScreenPos();
	const ImVec2 End(Origin.x + Size.x, Origin.y + Size.y);
	ImGui::InvisibleButton("##ConstraintGraphCanvas", Size, ImGuiButtonFlags_MouseButtonLeft |
		ImGuiButtonFlags_MouseButtonMiddle | ImGuiButtonFlags_MouseButtonRight);
	const bool bHovered = ImGui::IsItemHovered();
	const ImGuiIO& IO = ImGui::GetIO();
	if (bHovered && (ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
		ImGui::IsMouseClicked(ImGuiMouseButton_Middle) || ImGui::IsMouseClicked(ImGuiMouseButton_Right))) ImGui::SetWindowFocus();
	if (!IO.AppFocusLost && ImGui::IsItemActive() &&
		(ImGui::IsMouseDown(ImGuiMouseButton_Middle) || ImGui::IsMouseDown(ImGuiMouseButton_Right)))
	{
		Pan.x += IO.MouseDelta.x;
		Pan.y += IO.MouseDelta.y;
	}
	if (bHovered && IO.MouseWheel != 0.0f && !IO.AppFocusLost)
	{
		const float NewZoom = FMath::Clamp(Zoom * std::pow(1.15f, IO.MouseWheel), 0.2f, 2.5f);
		const float Ratio = NewZoom / Zoom;
		Pan.x = IO.MousePos.x - Origin.x - (IO.MousePos.x - Origin.x - Pan.x) * Ratio;
		Pan.y = IO.MousePos.y - Origin.y - (IO.MousePos.y - Origin.y - Pan.y) * Ratio;
		Zoom = NewZoom;
	}
	const auto ScreenPosition = [&](const ImVec2& Position)
	{
		return ImVec2(Origin.x + Pan.x + Position.x * Zoom, Origin.y + Pan.y + Position.y * Zoom);
	};

	ImDrawList* Draw = ImGui::GetWindowDrawList();
	Draw->PushClipRect(Origin, End, true);
	Draw->AddRectFilled(Origin, End, EditorColor::BgDeepest);
	const auto DrawLink = [&](const ImVec2& Start, const ImVec2& Finish, bool bSelected)
	{
		const float Tangent = FMath::Max(24.0f * Scale * Zoom, std::abs(Finish.x - Start.x) * 0.5f);
		Draw->AddBezierCubic(Start, ImVec2(Start.x + Tangent, Start.y),
			ImVec2(Finish.x - Tangent, Finish.y), Finish,
			bSelected ? EditorColor::AccentBright : EditorColor::CategoryBlue, (bSelected ? 3.0f : 2.0f) * Scale);
	};
	for (int32 Index = 0; Index < Asset->Constraints.Num(); ++Index)
	{
		if (!VisibleConstraints[Index]) continue;
		const int32 Parent = ConstraintParents[Index], Child = ConstraintChildren[Index];
		if (Parent < 0 || Child < 0) continue;
		const ImVec2 A = ScreenPosition(Positions[Parent]);
		const ImVec2 B = ScreenPosition(Positions[Child]);
		const ImVec2 C = ScreenPosition(ConstraintPositions[Index]);
		const bool bSelected = Index == Editor.GetSelectedConstraintIndex();
		DrawLink(ImVec2(A.x + NodeSize.x * Zoom, A.y + NodeSize.y * Zoom * 0.5f),
			ImVec2(C.x, C.y + ConstraintNodeSize.y * Zoom * 0.5f), bSelected);
		DrawLink(ImVec2(C.x + ConstraintNodeSize.x * Zoom, C.y + ConstraintNodeSize.y * Zoom * 0.5f),
			ImVec2(B.x, B.y + NodeSize.y * Zoom * 0.5f), bSelected);
	}

	const auto DrawNode = [&](const ImVec2& Position, const ImVec2& Extent, const char* Type,
		const FString& Label, ImU32 Category, bool bSelected)
	{
		const ImVec2 Min = ScreenPosition(Position);
		const ImVec2 Max(Min.x + Extent.x * Zoom, Min.y + Extent.y * Zoom);
		const bool bNodeHovered = bHovered && ImRect(Min, Max).Contains(IO.MousePos);
		const float Unit = Scale * Zoom;
		Draw->AddRectFilled(Min, Max, bSelected ? EditorColor::AccentDim :
			(bNodeHovered ? EditorColor::BgElevatedAlt : EditorColor::BgElevated), 5.0f * Unit);
		Draw->AddRect(Min, Max, bSelected ? EditorColor::AccentBright : Category, 5.0f * Unit);
		const ImVec4 Clip(Min.x + 8.0f * Unit, Min.y, Max.x - 8.0f * Unit, Max.y);
		Draw->AddText(ImGui::GetFont(), ImGui::GetFontSize() * Zoom * 0.8f,
			ImVec2(Min.x + 10.0f * Unit, Min.y + 6.0f * Unit), Category, Type, nullptr, 0.0f, &Clip);
		Draw->AddText(ImGui::GetFont(), ImGui::GetFontSize() * Zoom,
			ImVec2(Min.x + 10.0f * Unit, Min.y + 25.0f * Unit), EditorColor::TextPrimary, Label.c_str(), nullptr, 0.0f, &Clip);
		Draw->AddCircleFilled(ImVec2(Min.x, (Min.y + Max.y) * 0.5f), 3.5f * Unit, Category);
		Draw->AddCircleFilled(ImVec2(Max.x, (Min.y + Max.y) * 0.5f), 3.5f * Unit, Category);
		return bNodeHovered;
	};
	int32 HoveredConstraint = -1;
	for (int32 Index = 0; Index < Asset->Constraints.Num(); ++Index)
	{
		if (!VisibleConstraints[Index]) continue;
		if (ConstraintParents[Index] < 0 || ConstraintChildren[Index] < 0) continue;
		const FPhysicsAssetConstraint& Constraint = Asset->Constraints[Index];
		const FString Label = Constraint.ParentBone + " -> " + Constraint.ChildBone;
		if (DrawNode(ConstraintPositions[Index], ConstraintNodeSize, "Constraint", Label, EditorColor::CategoryBlue,
			Index == Editor.GetSelectedConstraintIndex())) HoveredConstraint = Index;
	}
	int32 HoveredBody = -1;
	for (int32 Index = 0; Index < Bodies.Num(); ++Index)
	{
		if (!VisibleBodies[Index]) continue;
		if (DrawNode(Positions[Index], NodeSize, "Body", Bodies[Index].BodySetup->BoneName, EditorColor::Success,
			Index == Editor.GetSelectedBodyIndex())) HoveredBody = Index;
	}
	Draw->PopClipRect();
	if (bHovered && !IO.AppFocusLost && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
	{
		if (HoveredBody >= 0) Editor.SelectBody(HoveredBody);
		else if (HoveredConstraint >= 0) Editor.SelectConstraint(HoveredConstraint);
		else Editor.SelectBone(-1);
	}
	if (bHovered && HoveredBody < 0 && HoveredConstraint >= 0)
	{
		const FPhysicsAssetConstraint& Constraint = Asset->Constraints[HoveredConstraint];
		ImGui::SetTooltip("%s -> %s\nClick to edit this constraint.", Constraint.ParentBone.c_str(), Constraint.ChildBone.c_str());
	}
}

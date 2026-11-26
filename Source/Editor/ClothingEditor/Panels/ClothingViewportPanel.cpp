#include "EnginePCH.h"
#include "Editor/ClothingEditor/Panels/ClothingViewportPanel.h"
#include "Editor/ClothingEditor/ClothingEditor.h"
#include "Components/SkinnedMeshComponent.h"
#include <functional>

FClothingViewportPanel::FClothingViewportPanel(FClothingEditor& InEditor)
	: FClothingEditorPanel(InEditor, "Viewport")
{
}

void FClothingViewportPanel::DrawClothingToolStrip()
{
	const EClothingViewportTool CurrentTool = Editor.GetViewportTool();

	const std::function<void(const char*, EClothingViewportTool)> DrawToolButton =
		[&](const char* Label, EClothingViewportTool Tool)
	{
		const bool bActive = (CurrentTool == Tool);
		if (bActive)
		{
			ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.22f, 0.50f, 0.78f, 1.0f));
		}
		if (ImGui::Button(Label))
		{
			Editor.SetViewportTool(Tool);
		}
		if (bActive)
		{
			ImGui::PopStyleColor();
		}
	};

	DrawToolButton("Pull Test", EClothingViewportTool::PullTest);
	ImGui::SameLine();
	DrawToolButton("Tear Brush", EClothingViewportTool::TearBrush);
	ImGui::SameLine();
	DrawToolButton("Paint Region", EClothingViewportTool::PaintRegion);
	ImGui::SameLine();
	DrawToolButton("Move Bone", EClothingViewportTool::PickBone);

	if (CurrentTool == EClothingViewportTool::PickBone)
	{
		const EGizmoMode GizmoMode = Editor.GetGizmoMode();
		const std::function<void(const char*, EGizmoMode)> DrawGizmoButton =
			[&](const char* Label, EGizmoMode Mode)
		{
			ImGui::SameLine();
			const bool bActive = (GizmoMode == Mode);
			if (bActive)
			{
				ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.28f, 0.56f, 0.34f, 1.0f));
			}
			if (ImGui::Button(Label))
			{
				Editor.SetGizmoMode(Mode);
			}
			if (bActive)
			{
				ImGui::PopStyleColor();
			}
		};
		ImGui::SameLine();
		ImGui::TextDisabled("[");
		DrawGizmoButton("Move (W)", EGizmoMode::Location);
		DrawGizmoButton("Rotate (E)", EGizmoMode::Rotation);
		DrawGizmoButton("Scale (R)", EGizmoMode::Scale);
		ImGui::SameLine();
		ImGui::TextDisabled("]");
	}

	ImGui::SameLine();
	ImGui::TextDisabled("|");
	ImGui::SameLine();

	bool bSimCloth = Editor.IsSimulatingClothPhysics();
	if (ImGui::Checkbox("Cloth Sim", &bSimCloth))
	{
		Editor.SetSimulateClothPhysics(bSimCloth);
	}

	ImGui::SameLine();
	bool bGpuSkin = USkinnedMeshComponent::bUseGPUSkinning;
	if (ImGui::Checkbox("GPU Skin", &bGpuSkin))
	{
		USkinnedMeshComponent::SetGPUSkinningEnabled(bGpuSkin);
	}

	ImGui::SameLine();
	if (ImGui::Button("Reset Cloth"))
	{
		Editor.ResetAllClothPoses();
	}

	ImGui::SameLine();
	if (ImGui::Button("Reset Pose"))
	{
		Editor.ResetCharacterPose();
	}

	ImGui::SameLine();
	if (ImGui::Button("Repair Tears (2)"))
	{
		Editor.RepairAllStockingTears();
	}
}

void FClothingViewportPanel::DrawContent()
{
	DrawPreviewViewToolbar(Editor, ToolbarState);
	DrawClothingToolStrip();

	FEditorViewportClient* Client = Editor.GetViewportClient();
	const ImVec2 Size = ImGui::GetContentRegionAvail();
	if (!Client || Size.x < 1.0f || Size.y < 1.0f)
	{
		return;
	}

	const ImVec2 Origin = ImGui::GetCursorScreenPos();

	ImGui::InvisibleButton("##ClothingViewport", Size,
		ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
	if (FTexture2D* Target = Client->GetColorTarget(); Target && Target->GetSRV())
	{
		ImGui::GetWindowDrawList()->AddImage(Target->GetSRV(), Origin,
			ImVec2(Origin.x + Size.x, Origin.y + Size.y));
	}

	const bool bHovered = ImGui::IsItemHovered();
	if (bHovered && (ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
		ImGui::IsMouseClicked(ImGuiMouseButton_Right)))
	{
		ImGui::SetWindowFocus();
	}

	const bool bFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
	if (bFocused && bHovered && ImGui::IsKeyPressed(ImGuiKey_F, false))
	{
		Editor.FocusMesh();
	}
	if (bFocused && bHovered && !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_2, false))
	{
		Editor.RepairAllStockingTears();
	}

	const ImGuiIO& IO = ImGui::GetIO();
	const bool bMouseDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);
	const bool bCameraDragging = ImGui::IsMouseDown(ImGuiMouseButton_Right);

	const FViewportPointerInput PointerInput
	{
		.MouseUV = FVector2((IO.MousePos.x - Origin.x) / Size.x, (IO.MousePos.y - Origin.y) / Size.y),
		.bHovered = bHovered,
		.bFocused = bFocused && !IO.AppFocusLost,
		.bMousePressed = bHovered && !bCameraDragging && ImGui::IsMouseClicked(ImGuiMouseButton_Left),
		.bMouseDown = bMouseDown,
		.bCancel = ImGui::IsKeyPressed(ImGuiKey_Escape, false),
	};

	const FViewportCameraInput CameraInput
	{
		.bCaptured = bFocused && ImGui::IsItemActive() && bCameraDragging && !bMouseDown && !Editor.IsGizmoDragging(),
		.bAllowWheel = bHovered && !bMouseDown && !Editor.IsGizmoDragging(),
		.bForward = ImGui::IsKeyDown(ImGuiKey_W),
		.bBackward = ImGui::IsKeyDown(ImGuiKey_S),
		.bRight = ImGui::IsKeyDown(ImGuiKey_D),
		.bLeft = ImGui::IsKeyDown(ImGuiKey_A),
		.bUp = ImGui::IsKeyDown(ImGuiKey_E),
		.bDown = ImGui::IsKeyDown(ImGuiKey_Q),
		.MouseX = IO.MouseDelta.x,
		.MouseY = IO.MouseDelta.y,
		.Wheel = IO.MouseWheel * 120.0f,
	};

	// 본 이동 모드에서 키보드로 기즈모 모드 전환
	if (Editor.GetViewportTool() == EClothingViewportTool::PickBone &&
		bFocused && bHovered && !bCameraDragging && !IO.WantTextInput && !IO.AppFocusLost)
	{
		if (ImGui::IsKeyPressed(ImGuiKey_W, false))
		{
			Editor.SetGizmoMode(EGizmoMode::Location);
		}
		if (ImGui::IsKeyPressed(ImGuiKey_E, false))
		{
			Editor.SetGizmoMode(EGizmoMode::Rotation);
		}
		if (ImGui::IsKeyPressed(ImGuiKey_R, false))
		{
			Editor.SetGizmoMode(EGizmoMode::Scale);
		}
		if (!Editor.IsGizmoDragging() && ImGui::IsKeyPressed(ImGuiKey_Space, false))
		{
			const int32 NextMode = (static_cast<int32>(Editor.GetGizmoMode()) + 1) % 3;
			Editor.SetGizmoMode(static_cast<EGizmoMode>(NextMode));
		}
	}

	Editor.SetViewportState(static_cast<uint32>(Size.x), static_cast<uint32>(Size.y), CameraInput, PointerInput);
}

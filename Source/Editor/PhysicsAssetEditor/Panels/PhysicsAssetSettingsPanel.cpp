#include "EnginePCH.h"
#include "PhysicsAssetSettingsPanel.h"
#include "Editor/PhysicsAssetEditor/PhysicsAssetEditor.h"
#include "Editor/Viewports/EditorViewportClient.h"

#include <cfloat>
#include <cmath>

FPhysicsAssetSettingsPanel::FPhysicsAssetSettingsPanel(FPhysicsAssetEditor& InEditor)
	: FPhysicsAssetEditorPanel(InEditor, "Settings")
{
}

void FPhysicsAssetSettingsPanel::DrawContent()
{
	if (ImGui::CollapsingHeader("Physics Bodies", ImGuiTreeNodeFlags_DefaultOpen))
	{
		const char* ModeNames[] = { "All Bodies", "Selected Body Only", "Hidden" };
		int Mode = static_cast<int>(Editor.GetBodyDrawMode());
		ImGui::TextUnformatted("Body Drawing");
		ImGui::SetNextItemWidth(-FLT_MIN);
		if (ImGui::Combo("##PhysicsBodyDrawing", &Mode, ModeNames, IM_ARRAYSIZE(ModeNames)))
			Editor.SetBodyDrawMode(static_cast<FPhysicsAssetEditor::EBodyDrawMode>(Mode));
		if (Editor.GetBodyDrawMode() == FPhysicsAssetEditor::EBodyDrawMode::SelectedOnly &&
			Editor.GetSelectedBodyIndex() < 0 && !Editor.GetSelectedConstraint())
			ImGui::TextWrapped("Select a body or constraint to display its bodies.");

		FVector4 BodyColor = Editor.GetBodyColor();
		ImGui::TextUnformatted("Body Color");
		ImGui::SetNextItemWidth(-FLT_MIN);
		if (ImGui::ColorEdit3("##PhysicsBodyColor", &BodyColor.X)) Editor.SetBodyColor(BodyColor);
		FVector4 SelectedBodyColor = Editor.GetSelectedBodyColor();
		ImGui::TextUnformatted("Selected Body Color");
		ImGui::SetNextItemWidth(-FLT_MIN);
		if (ImGui::ColorEdit3("##SelectedPhysicsBodyColor", &SelectedBodyColor.X)) Editor.SetSelectedBodyColor(SelectedBodyColor);
		FVector4 SelectedShapeColor = Editor.GetSelectedShapeColor();
		ImGui::TextUnformatted("Selected Shape Color");
		ImGui::SetNextItemWidth(-FLT_MIN);
		if (ImGui::ColorEdit3("##SelectedPhysicsShapeColor", &SelectedShapeColor.X)) Editor.SetSelectedShapeColor(SelectedShapeColor);

		bool bDepthTest = Editor.IsBodyDepthTestEnabled();
		if (ImGui::Checkbox("Depth Test", &bDepthTest)) Editor.SetBodyDepthTest(bDepthTest);
		ImGui::TextWrapped(bDepthTest ? "Bodies are occluded by scene geometry." : "Bodies are visible through scene geometry.");
	}

	if (ImGui::CollapsingHeader("Preview Scene", ImGuiTreeNodeFlags_DefaultOpen))
	{
		bool bShowMesh = Editor.IsMeshVisible();
		if (ImGui::Checkbox("Show Mesh", &bShowMesh))
		{
			Editor.SetMeshVisible(bShowMesh);
		}
		bool bShowGrid = Editor.IsGridVisible();
		if (ImGui::Checkbox("Show Grid", &bShowGrid))
		{
			Editor.SetGridVisible(bShowGrid);
		}

		ImGui::BeginDisabled(!Editor.GetMesh());
		if (ImGui::Button("Focus Preview Mesh (F)", ImVec2(-FLT_MIN, 0.0f)))
		{
			Editor.FocusMesh();
		}
		ImGui::EndDisabled();
	}

	if (ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen))
	{
		float CameraSpeed = Editor.GetCameraSpeed();
		ImGui::TextUnformatted("Movement Speed");
		ImGui::SetNextItemWidth(-FLT_MIN);
		if (ImGui::SliderFloat("##CameraSpeed", &CameraSpeed, 0.1f, 150.0f, "%.1f",
			ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic))
		{
			Editor.SetCameraSpeed(CameraSpeed);
		}

		if (FEditorViewportClient* Client = Editor.GetViewportClient())
		{
			const char* ProjectionNames[] = { "Perspective", "Top", "Front", "Right", "Bottom", "Left", "Back" };
			static_assert(IM_ARRAYSIZE(ProjectionNames) == static_cast<int>(ELevelViewportType::Count));
			int Projection = static_cast<int>(Client->GetViewportType());
			ImGui::TextUnformatted("Projection");
			ImGui::SetNextItemWidth(-FLT_MIN);
			if (ImGui::Combo("##Projection", &Projection, ProjectionNames, IM_ARRAYSIZE(ProjectionNames)))
			{
				Client->SetViewportType(static_cast<ELevelViewportType>(Projection));
				Editor.FocusMesh();
			}

			if (Client->IsPerspective())
			{
				float FOV = Client->GetViewFOV();
				ImGui::TextUnformatted("Field of View");
				ImGui::SetNextItemWidth(-FLT_MIN);
				if (ImGui::SliderFloat("##FOV", &FOV, 1.0f, 179.0f, "%.1f deg", ImGuiSliderFlags_AlwaysClamp)
					&& std::isfinite(FOV))
				{
					Client->SetViewFOV(FOV);
				}
			}
			else
			{
				float OrthoWidth = Client->GetOrthoWidth();
				ImGui::TextUnformatted("Orthographic Width");
				ImGui::SetNextItemWidth(-FLT_MIN);
				if (ImGui::DragFloat("##OrthoWidth", &OrthoWidth, 0.1f, 0.01f, 1000000.0f, "%.2f",
					ImGuiSliderFlags_AlwaysClamp) && std::isfinite(OrthoWidth))
				{
					Client->SetOrthoWidth(OrthoWidth);
				}
			}

			float NearClip = Client->GetNearClip();
			float FarClip = Client->GetFarClip();
			ImGui::TextUnformatted("Near Clip Plane");
			ImGui::SetNextItemWidth(-FLT_MIN);
			bool bClipChanged = ImGui::InputFloat("##NearClip", &NearClip, 0.0f, 0.0f, "%.3f");
			ImGui::TextUnformatted("Far Clip Plane");
			ImGui::SetNextItemWidth(-FLT_MIN);
			bClipChanged |= ImGui::InputFloat("##FarClip", &FarClip, 0.0f, 0.0f, "%.1f");
			if (bClipChanged)
			{
				if (std::isfinite(NearClip) && std::isfinite(FarClip) && NearClip > 0.0f && FarClip > NearClip)
				{
					Client->SetClipPlanes(NearClip, FarClip);
				}
				else
				{
					ImGui::TextWrapped("Clip planes must satisfy 0 < Near < Far.");
				}
			}
		}
		else
		{
			ImGui::TextDisabled("The preview viewport is unavailable.");
		}
	}

	if (ImGui::CollapsingHeader("Rendering", ImGuiTreeNodeFlags_DefaultOpen))
	{
		if (FEditorViewportClient* Client = Editor.GetViewportClient())
		{
			const EViewportMode Modes[] = {
				EViewportMode::Solid, EViewportMode::Unlit, EViewportMode::Wireframe,
				EViewportMode::SceneDepth, EViewportMode::WorldNormal,
				EViewportMode::LightCulling, EViewportMode::Cascade
			};
			const char* ModeNames[] = { "Lit", "Unlit", "Wireframe", "Scene Depth", "World Normal", "Light Culling", "Cascade" };
			static_assert(IM_ARRAYSIZE(Modes) == IM_ARRAYSIZE(ModeNames));
			int ModeIndex = -1;
			for (int Index = 0; Index < IM_ARRAYSIZE(Modes); ++Index)
			{
				if (Client->GetViewportMode() == Modes[Index])
				{
					ModeIndex = Index;
					break;
				}
			}
			ImGui::TextUnformatted("View Mode");
			ImGui::SetNextItemWidth(-FLT_MIN);
			if (ImGui::Combo("##ViewMode", &ModeIndex, ModeNames, IM_ARRAYSIZE(ModeNames)))
			{
				Client->SetViewportMode(Modes[ModeIndex]);
			}
		}
		else
		{
			ImGui::TextDisabled("The preview viewport is unavailable.");
		}
	}

	if (ImGui::CollapsingHeader("Skeleton", ImGuiTreeNodeFlags_DefaultOpen))
	{
		const char* BoneModeNames[] = { "All Hierarchy", "Selected Only", "Selected and Parents", "None" };
		int BoneMode = static_cast<int>(Editor.GetBoneDrawMode());
		ImGui::TextUnformatted("Bone Drawing");
		ImGui::SetNextItemWidth(-FLT_MIN);
		if (ImGui::Combo("##BoneDrawMode", &BoneMode, BoneModeNames, IM_ARRAYSIZE(BoneModeNames)))
		{
			Editor.SetBoneDrawMode(static_cast<EBoneDrawMode>(BoneMode));
		}

		FVector4 BoneColor = Editor.GetBoneVisualizerColor();
		ImGui::TextUnformatted("Bone Color");
		ImGui::SetNextItemWidth(-FLT_MIN);
		if (ImGui::ColorEdit3("##BoneColor", &BoneColor.X))
		{
			Editor.SetBoneVisualizerColor(BoneColor);
		}

		bool bWeightHeatmap = Editor.IsWeightHeatmapEnabled();
		if (ImGui::Checkbox("Selected Bone Weight", &bWeightHeatmap))
		{
			Editor.SetWeightHeatmap(bWeightHeatmap);
		}
		if (bWeightHeatmap && Editor.GetSelectedBoneIndex() < 0)
		{
			ImGui::TextWrapped("Select a bone to display its skin weights.");
		}
	}
}

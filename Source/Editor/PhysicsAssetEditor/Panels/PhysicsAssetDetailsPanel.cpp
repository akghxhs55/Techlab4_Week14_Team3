#include "EnginePCH.h"
#include "PhysicsAssetDetailsPanel.h"
#include "Editor/PhysicsAssetEditor/PhysicsAssetEditor.h"
#include "Editor/Details/PropertyEditor.h"
#include "Components/SkeletalMeshComponent.h"
#include "Render/SkeletalMesh.h"
#include "Physics/PhysicsAsset.h"

#include <cfloat>

FPhysicsAssetDetailsPanel::FPhysicsAssetDetailsPanel(FPhysicsAssetEditor& InEditor)
	: FPhysicsAssetEditorPanel(InEditor, "Details")
{
}

void FPhysicsAssetDetailsPanel::DrawContent()
{
	const USkeletalMesh* Mesh = Editor.GetMesh();
	if (!Mesh)
	{
		ImGui::TextWrapped("No preview skeletal mesh is available.");
		return;
	}

	if (const FPhysicsAssetConstraint* Selected = Editor.GetSelectedConstraint())
	{
		ImGui::BeginDisabled(Editor.IsSimulationActive());
		FPhysicsAssetConstraint Properties = *Selected;
		ImGui::PushID("ConstraintProperties");
		ImGui::PushID(Editor.GetSelectedConstraintIndex());
		ImGui::PushID(static_cast<int>(Editor.GetTopologyRevision()));
		ImGui::TextDisabled("Parent Body");
		ImGui::TextWrapped("%s", Properties.ParentBone.c_str());
		ImGui::TextDisabled("Child Body");
		ImGui::TextWrapped("%s", Properties.ChildBone.c_str());
		ImGui::Separator();
		ImGui::TextDisabled("Linear Motion: Locked");
		const auto DrawMotion = [](const char* Label, EConstraintMotion& Motion)
		{
			const char* Names[] = { "Locked", "Limited", "Free" };
			int Value = static_cast<int>(Motion);
			if (!ImGui::Combo(Label, &Value, Names, IM_ARRAYSIZE(Names))) return false;
			Motion = static_cast<EConstraintMotion>(Value);
			return true;
		};
		bool bChanged = DrawMotion("Swing 1", Properties.Swing1Motion);
		ImGui::BeginDisabled(Properties.Swing1Motion != EConstraintMotion::Limited);
		bChanged |= ImGui::SliderFloat("Swing 1 Limit", &Properties.Swing1LimitDeg, 0.1f, 179.0f, "%.1f deg", ImGuiSliderFlags_AlwaysClamp);
		ImGui::EndDisabled();
		bChanged |= DrawMotion("Swing 2", Properties.Swing2Motion);
		ImGui::BeginDisabled(Properties.Swing2Motion != EConstraintMotion::Limited);
		bChanged |= ImGui::SliderFloat("Swing 2 Limit", &Properties.Swing2LimitDeg, 0.1f, 179.0f, "%.1f deg", ImGuiSliderFlags_AlwaysClamp);
		ImGui::EndDisabled();
		bChanged |= DrawMotion("Twist", Properties.TwistMotion);
		ImGui::BeginDisabled(Properties.TwistMotion != EConstraintMotion::Limited);
		bChanged |= ImGui::DragFloatRange2("Twist Limits", &Properties.TwistMinDeg, &Properties.TwistMaxDeg,
			0.5f, -179.0f, 179.0f, "Min %.1f deg", "Max %.1f deg", ImGuiSliderFlags_AlwaysClamp);
		ImGui::EndDisabled();
		bChanged |= FPropertyEditor::DrawRotator("Frame Offset", Properties.FrameOffset);
		if (bChanged && !Editor.SetSelectedConstraintProperties(Properties))
			ImGui::TextWrapped("Constraint values must be finite numbers.");
		ImGui::PopID();
		ImGui::PopID();
		ImGui::PopID();
		if (ImGui::Button("Delete Constraint")) Editor.RemoveConstraint(Editor.GetSelectedConstraintIndex());
		ImGui::EndDisabled();
		ImGui::Separator();
		ImGui::TextWrapped("Viewport: Green = Swing limits, Blue = Twist limits, Yellow = Current orientation, Red = Locked, Gray = Free / Reference.");
		if (Editor.GetBodyDrawMode() == FPhysicsAssetEditor::EBodyDrawMode::None)
			ImGui::TextWrapped("Enable Body Drawing in Settings to display constraint limits.");
		return;
	}

	if (ImGui::CollapsingHeader("Preview Mesh", ImGuiTreeNodeFlags_DefaultOpen))
	{
		ImGui::TextDisabled("Mesh Path");
		ImGui::TextWrapped("%s", Mesh->GetPath().c_str());
		ImGui::Text("Bones: %d", Mesh->GetNumBones());
	}

	const int32 BoneIndex = Editor.GetSelectedBoneIndex();
	const TArray<FBoneInfo>& Bones = Mesh->GetRefSkeleton();
	const bool bValidBone = BoneIndex >= 0 && BoneIndex < Bones.Num();
	if (ImGui::CollapsingHeader("Selection", ImGuiTreeNodeFlags_DefaultOpen))
	{
		if (bValidBone)
		{
			const FBoneInfo& Bone = Bones[BoneIndex];
			ImGui::TextDisabled("Bone Name");
			ImGui::TextWrapped("%s", Bone.Name.c_str());
			ImGui::Text("Bone Index: %d", BoneIndex);
			ImGui::TextDisabled("Parent Bone");
			const bool bValidParent = Bone.ParentIndex >= 0 && Bone.ParentIndex < Bones.Num();
			ImGui::TextWrapped("%s", bValidParent ? Bones[Bone.ParentIndex].Name.c_str() : "None (Root)");
		}
		else
		{
			ImGui::TextWrapped("Select a bone in the viewport or skeleton tree to inspect it.");
		}
	}

	if (ImGui::CollapsingHeader("Shape", ImGuiTreeNodeFlags_DefaultOpen))
	{
		FShapeElem Shape;
		FVector Size;
		FPhysicsAssetEditor::EPreviewShapeType Type;
		if (Editor.GetSelectedShapeProperties(Shape, Size, Type))
		{
			// 선택이 바뀌면 입력 중인 텍스트나 드래그 값이 다른 형상으로 이어지지 않도록 ID를 구분합니다.
			ImGui::PushID("SelectedShape");
			ImGui::PushID(Editor.GetSelectedBodyIndex());
			ImGui::PushID(static_cast<int>(Type));
			ImGui::PushID(Editor.GetSelectedShapeIndex());
			ImGui::PushID(static_cast<int>(Editor.GetTopologyRevision()));
			ImGui::TextDisabled("Bone Space / Rotation in Degrees");
			ImGui::BeginDisabled(Editor.IsGizmoDragging() || Editor.IsSimulationActive());
			bool bChanged = FPropertyEditor::DrawVector3("Location", Shape.Center.V, 0.0f);
			bChanged |= FPropertyEditor::DrawRotator("Rotation", Shape.Rotation);

			switch (Type)
			{
			case FPhysicsAssetEditor::EPreviewShapeType::Sphere:
				ImGui::TextUnformatted("Sphere Radius");
				ImGui::SetNextItemWidth(-FLT_MIN);
				bChanged |= ImGui::DragFloat("##SphereRadius", &Size.X, 0.01f, 0.001f, FLT_MAX, "%.3f", ImGuiSliderFlags_AlwaysClamp);
				break;
			case FPhysicsAssetEditor::EPreviewShapeType::Box:
				bChanged |= FPropertyEditor::DrawVector3("Box Size", Size.V, 1.0f);
				ImGui::TextDisabled("Full dimensions (X, Y, Z)");
				break;
			case FPhysicsAssetEditor::EPreviewShapeType::Capsule:
				ImGui::TextUnformatted("Capsule Radius");
				ImGui::SetNextItemWidth(-FLT_MIN);
				bChanged |= ImGui::DragFloat("##CapsuleRadius", &Size.X, 0.01f, 0.001f, FLT_MAX, "%.3f", ImGuiSliderFlags_AlwaysClamp);
				ImGui::TextUnformatted("Cylinder Length");
				ImGui::SetNextItemWidth(-FLT_MIN);
				bChanged |= ImGui::DragFloat("##CapsuleLength", &Size.Y, 0.01f, 0.0f, FLT_MAX, "%.3f", ImGuiSliderFlags_AlwaysClamp);
				ImGui::TextDisabled("Length excludes both hemispheres.");
				break;
			default: break;
			}
			ImGui::EndDisabled();
			if (bChanged && !Editor.SetSelectedShapeProperties(Shape, Size))
				ImGui::TextWrapped("Values must be finite numbers.");
			ImGui::PopID();
			ImGui::PopID();
			ImGui::PopID();
			ImGui::PopID();
			ImGui::PopID();
		}
		else
		{
			ImGui::TextWrapped("Select a body node or a collision shape to edit its properties.");
		}
	}

	if (bValidBone && ImGui::CollapsingHeader("Bone Transform", ImGuiTreeNodeFlags_DefaultOpen))
	{
		if (const USkeletalMeshComponent* Component = Editor.GetMeshComponent())
		{
			// 본 포즈와 충돌 형상 변환을 혼동하지 않도록 본의 변환은 읽기 전용으로 표시합니다.
			FTransform Transform = Component->GetBoneLocalTransform(BoneIndex);
			FRotator Rotation = Transform.Rotation.ToFRotator();
			ImGui::TextDisabled("Parent Space (Read Only)");
			ImGui::PushID(BoneIndex);
			ImGui::BeginDisabled();
			FPropertyEditor::DrawVector3("Location", Transform.Location.V, 0.0f);
			FPropertyEditor::DrawRotator("Rotation", Rotation);
			FPropertyEditor::DrawVector3("Scale", Transform.Scale.V, 1.0f);
			ImGui::EndDisabled();
			ImGui::PopID();
		}
		else
		{
			ImGui::TextWrapped("The preview mesh component is unavailable.");
		}
	}

	const FPhysicsAssetPreviewBody* Body = Editor.GetSelectedBody();
	if (ImGui::CollapsingHeader("Body Setup", ImGuiTreeNodeFlags_DefaultOpen))
	{
		if (Body)
		{
			ImGui::Text("Body Index: %d", Editor.GetSelectedBodyIndex());
			ImGui::Text("Shapes: %d", Body->GetGeometry().GetElementCount());
			ImGui::TextWrapped("Save this Physics Asset with Ctrl+S.");
		}
		else
		{
			ImGui::TextWrapped("Select a body node or a collision shape. Right-click a bone in the skeleton tree to add a body.");
		}
	}
	if (ImGui::CollapsingHeader("Collision"))
	{
		if (Body)
		{
			ImGui::Text("Spheres: %d", Body->GetGeometry().SphereElems.Num());
			ImGui::Text("Boxes: %d", Body->GetGeometry().BoxElems.Num());
			ImGui::Text("Capsules: %d", Body->GetGeometry().CapsuleElems.Num());
		}
		else
		{
			ImGui::TextWrapped("Select a body to inspect its collision geometry.");
		}
	}
	if (ImGui::CollapsingHeader("Constraint"))
	{
		const int32 ChildBody = Editor.GetSelectedBodyIndex();
		const int32 ParentBody = Editor.GetNearestParentBody(ChildBody);
		const int32 ConstraintIndex = Editor.FindConstraintForBody(ChildBody);
		if (ConstraintIndex >= 0)
		{
			if (ImGui::Button("Edit Constraint")) Editor.SelectConstraint(ConstraintIndex);
		}
		else if (ParentBody >= 0)
		{
			const int32 ParentBone = Editor.GetPreviewBodies()[ParentBody].BoneIndex;
			ImGui::TextWrapped("Parent Body: %s", Bones[ParentBone].Name.c_str());
			ImGui::BeginDisabled(!Editor.CanAddConstraint(ParentBody, ChildBody));
			if (ImGui::Button("Connect to Parent")) Editor.AddConstraint(ParentBody, ChildBody);
			ImGui::EndDisabled();
			ImGui::TextWrapped("Both bodies must have collision shapes.");
		}
		else
		{
			ImGui::TextWrapped("Select a child body with an ancestor body to create a constraint.");
		}
	}
}

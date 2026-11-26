#include "EnginePCH.h"
#include "Editor/ClothingEditor/Panels/ClothingInspectorPanel.h"
#include "Editor/ClothingEditor/ClothingEditor.h"
#include "Components/ClothComponent.h"
#include "Components/StockingComponent.h"
#include "Editor/Details/PropertyEditor.h"
#include "Render/SkeletalMesh.h"
#include "Render/StockingRegion.h"

#include <cstdio>

FClothingInspectorPanel::FClothingInspectorPanel(FClothingEditor& InEditor)
	: FClothingEditorPanel(InEditor, "Inspector")
{
}

void FClothingInspectorPanel::DrawContent()
{
	const int32 LayerIndex = Editor.GetSelectedLayerIndex();
	FClothingLayer* Layer = Editor.GetSelectedLayer();
	if (!Layer)
	{
		ImGui::TextDisabled("Select or add a clothing layer to inspect.");
	}
	else if (Layer->Kind == EClothingLayerKind::Stocking)
	{
		DrawStockingInspector(LayerIndex, *Layer);
	}
	else
	{
		DrawClothInspector(LayerIndex, *Layer);
	}

	ImGui::Separator();
	DrawBonePoseTestSection();
	ImGui::Separator();
	DrawDebugDisplaySection();
}

void FClothingInspectorPanel::DrawStockingInspector(int32 LayerIndex, FClothingLayer& Layer)
{
	UStockingRegion* Region = Layer.StockingRegion;
	UStockingComponent* Stocking = Layer.StockingComponent;
	if (!Region || !Stocking)
	{
		return;
	}

	const float S = Editor.GetCharacterScale();
	ImGui::Text("Stocking Layer: %s (Scale x%.2f)", Layer.Name.c_str(), S);

	if (ImGui::CollapsingHeader("Region & Mesh", ImGuiTreeNodeFlags_DefaultOpen))
	{
		bool bErase = Editor.IsRegionEraseMode();
		if (ImGui::Checkbox("Erase Brush", &bErase))
		{
			Editor.SetRegionEraseMode(bErase);
		}
		float BrushRadius = Editor.GetRegionBrushRadius();
		if (ImGui::SliderFloat("Brush Radius", &BrushRadius, 0.0f, 0.45f * S, "%.3f"))
		{
			Editor.SetRegionBrushRadius(BrushRadius);
		}

		bool bNeedsRebuild = false;
		bNeedsRebuild |= ImGui::SliderInt("Subdivision", &Region->SubdivisionIterations, 0, 2);
		bNeedsRebuild |= ImGui::DragFloat("Boundary Feather", &Region->BoundaryFeatherDistance, 0.002f * S, 0.005f * S, 0.35f * S, "%.3f");

		if (ImGui::DragFloat("Surface Offset", &Stocking->SurfaceOffset, 0.0005f * S, 0.0f, 0.05f * S, "%.4f"))
		{
			Region->SurfaceOffset = Stocking->SurfaceOffset;
			Stocking->MarkBoneTransformsDirty();
		}

		float LegRadiusRatio = Editor.GetAutoLegMaxRadiusRatio();
		float LegHeightRatio = Editor.GetAutoLegMaxHeightRatio();
		bool bLegFilterChanged = false;
		if (ImGui::SliderFloat("Leg Radius Ratio", &LegRadiusRatio, 0.03f, 0.22f, "%.3f"))
		{
			Editor.SetAutoLegMaxRadiusRatio(LegRadiusRatio);
			bLegFilterChanged = true;
		}
		if (ImGui::SliderFloat("Leg Height Ratio", &LegHeightRatio, 0.20f, 1.15f, "%.2f"))
		{
			Editor.SetAutoLegMaxHeightRatio(LegHeightRatio);
			bLegFilterChanged = true;
		}

		if (ImGui::Button("Auto Legs") || bLegFilterChanged)
		{
			Editor.AutoSelectLegTriangles(LayerIndex);
		}
		ImGui::SameLine();
		if (ImGui::Button("Add Bone Tris"))
		{
			Editor.SelectStockingTrianglesByBone(LayerIndex, Editor.GetSelectedBoneIndex());
		}
		ImGui::SameLine();
		if (ImGui::Button("Grow"))
		{
			Editor.GrowStockingLayerRegion(LayerIndex);
		}
		ImGui::SameLine();
		if (ImGui::Button("Shrink"))
		{
			Editor.ShrinkStockingLayerRegion(LayerIndex);
		}
		ImGui::SameLine();
		if (ImGui::Button("Clear"))
		{
			Editor.ClearStockingLayerRegion(LayerIndex);
		}

		if (ImGui::Button("Rebuild Mesh") || bNeedsRebuild)
		{
			FString Msg;
			Editor.RebuildStockingLayer(LayerIndex, Msg);
		}
		ImGui::SameLine();
		if (ImGui::Button("Validate"))
		{
			FString Msg;
			Editor.ValidateStockingLayer(LayerIndex, Msg);
		}
		ImGui::SameLine();
		if (ImGui::Button("Save Region"))
		{
			FString Msg;
			Editor.SaveStockingLayerRegion(LayerIndex, Msg);
		}
		ImGui::SameLine();
		if (ImGui::Button("Load Region"))
		{
			FString Msg;
			Editor.LoadStockingLayerRegion(LayerIndex, FString(), Msg);
		}

		ImGui::Text("Source Triangles: %d", Region->GetSelectedTriangleCount());
		if (USkeletalMesh* BuiltMesh = Stocking->GetSkeletalMesh();
			BuiltMesh && !BuiltMesh->GetRenderData().LODRenderData.IsEmpty())
		{
			const FSkeletalMeshLODRenderData& LOD = BuiltMesh->GetRenderData().LODRenderData[0];
			ImGui::Text("Stocking Verts: %d | Tris: %d", LOD.BindVertices.Num(), LOD.Indices.Num() / 3);
		}

		const ImVec4 StatusColor = Layer.bRegionValid
			? ImVec4(0.35f, 0.90f, 0.45f, 1.0f)
			: ImVec4(0.95f, 0.55f, 0.25f, 1.0f);
		ImGui::TextColored(StatusColor, "%s", Layer.LastStatusMessage.c_str());
	}

	if (ImGui::CollapsingHeader("Pull & Wrinkles", ImGuiTreeNodeFlags_DefaultOpen))
	{
		ImGui::SliderFloat("Pull Radius", &Stocking->PullRadius, 0.02f * S, 0.35f * S, "%.3f");
		ImGui::SliderFloat("Max Pull Distance", &Stocking->MaxPullDistance, 0.01f * S, 0.25f * S, "%.3f");
		ImGui::SliderFloat("Pinch Falloff", &Stocking->PullFalloffExponent, 0.5f, 4.0f, "%.2f");
		ImGui::SliderFloat("Wrinkle Amplitude", &Stocking->WrinkleAmplitude, 0.0f, 0.03f * S, "%.4f");
		ImGui::SliderFloat("Wrinkle Folds", &Stocking->WrinkleFrequency, 2.0f, 16.0f, "%.1f");
		ImGui::SliderFloat("Spring Stiffness", &Stocking->SpringStiffness, 20.0f, 400.0f, "%.0f");
		ImGui::SliderFloat("Spring Damping", &Stocking->SpringDamping, 2.0f, 40.0f, "%.1f");
		ImGui::SliderFloat("Bounds Padding", &Stocking->BoundsPadding, 0.0f, 0.25f * S, "%.3f");
		ImGui::Text("Peak Stretch Ratio (Color.a): %.3f", Stocking->GetMaxCurrentStretchRatio());
		if (ImGui::Button("Reset Pull"))
		{
			Stocking->ResetPullImmediately();
		}
		ImGui::SameLine();
		if (ImGui::Button("Auto Scale Defaults"))
		{
			Stocking->CalibrateParametersForScale(S);
		}
	}

	if (ImGui::CollapsingHeader("NvCloth Joint Wrinkles", ImGuiTreeNodeFlags_DefaultOpen))
	{
		bool bClothChanged = false;
		if (ImGui::Checkbox("Use NvCloth Simulation", &Stocking->bUseNvClothSimulation))
		{
			Stocking->ResetClothSimulation();
		}
		ImGui::SliderFloat("Joint Wrinkle Slack", &Stocking->ClothJointWrinkleSlack, 0.0f, 0.02f * S, "%.4f");
		ImGui::SliderFloat("Backstop Radius", &Stocking->ClothBackstopRadius, 0.008f * S, 0.08f * S, "%.3f");
		bClothChanged |= ImGui::SliderFloat("Stretch Stiffness", &Stocking->ClothStretchStiffness, 0.1f, 1.0f, "%.2f");
		bClothChanged |= ImGui::SliderFloat("Bend Stiffness", &Stocking->ClothBendStiffness, 0.0f, 1.0f, "%.2f");
		bClothChanged |= ImGui::SliderFloat("Shear Stiffness", &Stocking->ClothShearStiffness, 0.0f, 1.0f, "%.2f");
		bClothChanged |= ImGui::SliderFloat("Compression Limit", &Stocking->ClothCompressionLimit, 0.6f, 1.0f, "%.2f");
		bClothChanged |= ImGui::SliderFloat("Stretch Limit", &Stocking->ClothStretchLimit, 1.0f, 1.5f, "%.2f");
		bClothChanged |= ImGui::SliderFloat("Motion Stiffness", &Stocking->ClothMotionStiffness, 0.05f, 1.0f, "%.2f");
		bClothChanged |= ImGui::SliderFloat("Cloth Damping", &Stocking->ClothDamping, 0.02f, 0.9f, "%.2f");
		bClothChanged |= ImGui::SliderFloat("Self-Collision Dist", &Stocking->ClothSelfCollisionDistance, 0.0f, 0.02f * S, "%.4f");
		if (bClothChanged)
		{
			Stocking->ApplyNvClothParams();
		}
		if (ImGui::Button("Reset Cloth State"))
		{
			Stocking->ResetClothSimulation();
		}
	}

	if (ImGui::CollapsingHeader("Stocking Tearing", ImGuiTreeNodeFlags_DefaultOpen))
	{
		FStockingMaterialParams& TearMat = Layer.StockingMatParams;
		bool bTearMatChanged = false;
		ImGui::Checkbox("Enable Tearing", &Stocking->bEnableTearing);
		ImGui::SliderFloat("Tear Stretch Threshold", &Stocking->TearStretchThreshold, 1.08f, 2.20f, "%.2f");
		ImGui::SliderFloat("Tear Spread Speed", &Stocking->TearSpreadSpeed, 0.2f, 3.0f, "%.2f");
		ImGui::SliderFloat("Ladder Run Length", &Stocking->TearRunLength, 0.05f, 0.70f, "%.2f");
		ImGui::SliderFloat("Tear Brush Size", &Stocking->TearBrushRadiusUV, 0.005f, 0.050f, "%.3f");
		ImGui::Checkbox("Ragdoll Impact Tear", &Stocking->bEnableRagdollImpactTearing);
		ImGui::SliderFloat("Impact Impulse Threshold", &Stocking->ImpactTearImpulseThreshold, 0.5f, 50.0f, "%.1f");
		ImGui::SliderFloat("Scrape Sensitivity", &Stocking->ImpactScrapeSensitivity, 0.0f, 2.0f, "%.2f");
		ImGui::SliderFloat("Impact Tear Cooldown", &Stocking->ImpactTearCooldownDuration, 0.02f, 0.60f, "%.2f");
		bTearMatChanged |= ImGui::SliderFloat("Tear Rim Strength", &TearMat.TearRimStrength, 0.0f, 1.5f, "%.2f");
		bTearMatChanged |= ImGui::SliderFloat("Skin Contact Shadow", &TearMat.TearSkinContactStrength, 0.0f, 1.5f, "%.2f");
		bTearMatChanged |= ImGui::SliderFloat("Ladder Rung Detail", &TearMat.TearRungStrength, 0.0f, 1.5f, "%.2f");
		if (bTearMatChanged)
		{
			Editor.ApplyStockingMaterialParams(LayerIndex);
		}
		ImGui::Text("Active Tears: %d", Stocking->GetActiveTearCount());
		if (ImGui::Button("Repair Tears"))
		{
			Stocking->RepairAllTears();
		}
	}

	if (ImGui::CollapsingHeader("Stocking Material", ImGuiTreeNodeFlags_DefaultOpen))
	{
		FStockingMaterialParams& P = Layer.StockingMatParams;
		bool bMatChanged = false;
		bMatChanged |= ImGui::ColorEdit3("Fabric Color", &P.FabricColor.X);
		bMatChanged |= ImGui::ColorEdit3("Hem Color", &P.HemColor.X);
		bMatChanged |= ImGui::SliderFloat("Base Opacity", &P.BaseOpacity, 0.05f, 1.0f, "%.2f");
		bMatChanged |= ImGui::SliderFloat("Fresnel Opacity", &P.FresnelOpacity, 0.0f, 1.0f, "%.2f");
		bMatChanged |= ImGui::SliderFloat("Fresnel Power", &P.FresnelPower, 0.5f, 6.0f, "%.2f");
		bMatChanged |= ImGui::SliderFloat("Stretch Sensitivity", &P.StretchSensitivity, 0.1f, 8.0f, "%.2f");
		bMatChanged |= ImGui::SliderFloat("Stretch Alpha Drop", &P.StretchAlphaDrop, 0.0f, 1.0f, "%.2f");
		bMatChanged |= ImGui::SliderFloat("Min Opacity", &P.MinOpacity, 0.01f, 0.6f, "%.2f");
		bMatChanged |= ImGui::SliderFloat("Weave Scale", &P.WeaveScale, 20.0f, 300.0f, "%.1f");
		bMatChanged |= ImGui::SliderFloat("Weave Strength", &P.WeaveStrength, 0.0f, 0.5f, "%.2f");
		bMatChanged |= ImGui::SliderFloat("Hem Feather Power", &P.HemFeatherPower, 0.1f, 3.0f, "%.2f");
		bMatChanged |= ImGui::SliderFloat("Hem Band Strength", &P.HemBandStrength, 0.0f, 0.6f, "%.2f");
		bMatChanged |= ImGui::SliderFloat("Sheen Strength", &P.SheenStrength, 0.0f, 0.5f, "%.2f");
		bMatChanged |= ImGui::SliderFloat("Silk Specular", &P.SpecularIntensity, 0.0f, 1.0f, "%.2f");
		bMatChanged |= ImGui::SliderFloat("Specular Sharpness", &P.SpecularPower, 4.0f, 96.0f, "%.1f");
		bMatChanged |= ImGui::SliderFloat("Wrinkle Shading", &P.WrinkleShadingStrength, 0.0f, 1.5f, "%.2f");

		if (bMatChanged)
		{
			Editor.ApplyStockingMaterialParams(LayerIndex);
		}
	}
}

void FClothingInspectorPanel::DrawClothInspector(int32 LayerIndex, FClothingLayer& Layer)
{
	UClothComponent* Cloth = Layer.ClothComponent;
	USkeletalMesh* Mesh = Editor.GetMesh();
	if (!Cloth)
	{
		return;
	}

	ImGui::Text("Cloth Layer: %s", Layer.Name.c_str());

	if (ImGui::CollapsingHeader("Geometry & Attachment", ImGuiTreeNodeFlags_DefaultOpen))
	{
		bool bGeometryChanged = false;
		bGeometryChanged |= FPropertyEditor::DrawVector3("Local Offset", Layer.ClothLocalOffset.V, 0.0f);

		if (Mesh && ImGui::BeginCombo("Pin Bone", Layer.PinBoneName.c_str()))
		{
			const TArray<FBoneInfo>& Bones = Mesh->GetRefSkeleton();
			for (int32 i = 0; i < Bones.Num(); ++i)
			{
				const bool bSelected = (Bones[i].Name == Layer.PinBoneName);
				if (ImGui::Selectable(Bones[i].Name.c_str(), bSelected))
				{
					Layer.PinBoneName = Bones[i].Name;
					bGeometryChanged = true;
				}
			}
			ImGui::EndCombo();
		}

		if (Layer.Kind == EClothingLayerKind::ClothCape)
		{
			bGeometryChanged |= ImGui::DragFloat("Top Width", &Cloth->ClothWidth, 0.01f, 0.05f, 3.0f, "%.3f");
			bGeometryChanged |= ImGui::DragFloat("Bottom Width", &Cloth->BottomWidth, 0.01f, 0.0f, 3.0f, "%.3f");
			bGeometryChanged |= ImGui::DragFloat("Height", &Cloth->ClothHeight, 0.01f, 0.05f, 3.0f, "%.3f");
			bGeometryChanged |= ImGui::SliderInt("Segments X", &Cloth->SegmentsX, 4, 32);
			bGeometryChanged |= ImGui::SliderInt("Segments Y", &Cloth->SegmentsY, 4, 32);
		}
		else if (Layer.Kind == EClothingLayerKind::ClothSkirt)
		{
			bGeometryChanged |= ImGui::DragFloat("Top Radius", &Cloth->TopRadius, 0.005f, 0.02f, 1.5f, "%.3f");
			bGeometryChanged |= ImGui::DragFloat("Bottom Radius", &Cloth->BottomRadius, 0.005f, 0.02f, 2.0f, "%.3f");
			bGeometryChanged |= ImGui::DragFloat("Height", &Cloth->ClothHeight, 0.01f, 0.05f, 2.5f, "%.3f");
			bGeometryChanged |= ImGui::SliderInt("Radial Segments", &Cloth->SegmentsX, 8, 48);
			bGeometryChanged |= ImGui::SliderInt("Height Segments", &Cloth->SegmentsY, 4, 24);
		}
		else if (Layer.Kind == EClothingLayerKind::ClothStaticMesh)
		{
			char PathBuf[260] = {};
			std::snprintf(PathBuf, sizeof(PathBuf), "%s", Layer.StaticMeshAssetPath.c_str());
			if (ImGui::InputText("StaticMesh Path", PathBuf, sizeof(PathBuf)))
			{
				Layer.StaticMeshAssetPath = PathBuf;
			}
			bGeometryChanged |= ImGui::SliderFloat("Pin Height Ratio", &Cloth->PinHeightRatio, 0.0f, 1.0f, "%.2f");
		}

		bGeometryChanged |= ImGui::Checkbox("Double Sided", &Cloth->bDoubleSidedMesh);
		bGeometryChanged |= ImGui::Checkbox("Pin Top Row", &Cloth->bPinTopRow);

		if (ImGui::Button("Rebuild Cloth") || bGeometryChanged)
		{
			Editor.RebuildClothLayer(LayerIndex);
		}
		ImGui::SameLine();
		if (ImGui::Button("Reset Cloth Pose"))
		{
			Cloth->ResetClothPose();
		}
	}

	if (ImGui::CollapsingHeader("Cloth Simulation", ImGuiTreeNodeFlags_DefaultOpen))
	{
		if (ImGui::Checkbox("Enable Simulation", &Cloth->bEnableClothSimulation))
		{
			Editor.ApplyClothLayerParams(LayerIndex, "bEnableClothSimulation");
		}
		bool bParamsChanged = false;
		bParamsChanged |= FPropertyEditor::DrawVector3("Gravity", Cloth->Gravity.V, 0.0f);
		bParamsChanged |= FPropertyEditor::DrawVector3("Wind Velocity", Cloth->WindVelocity.V, 0.0f);
		bParamsChanged |= FPropertyEditor::DrawVector3("Damping", Cloth->Damping.V, 0.05f);
		bParamsChanged |= ImGui::SliderFloat("Stretch Stiffness", &Cloth->StretchStiffness, 0.0f, 1.0f, "%.2f");
		bParamsChanged |= ImGui::SliderFloat("Bend Stiffness", &Cloth->BendStiffness, 0.0f, 1.0f, "%.2f");
		bParamsChanged |= ImGui::SliderFloat("Shear Stiffness", &Cloth->ShearStiffness, 0.0f, 1.0f, "%.2f");
		bParamsChanged |= ImGui::SliderFloat("Tether Scale", &Cloth->TetherScale, 0.5f, 2.0f, "%.2f");
		bParamsChanged |= ImGui::SliderFloat("Tether Stiffness", &Cloth->TetherStiffness, 0.0f, 1.0f, "%.2f");
		bParamsChanged |= ImGui::SliderFloat("Solver Frequency", &Cloth->SolverFrequency, 60.0f, 360.0f, "%.0f");
		bParamsChanged |= ImGui::SliderFloat("Drag Coeff", &Cloth->DragCoefficient, 0.0f, 1.0f, "%.2f");
		bParamsChanged |= ImGui::SliderFloat("Lift Coeff", &Cloth->LiftCoefficient, 0.0f, 1.0f, "%.2f");
		bParamsChanged |= ImGui::SliderFloat("Linear Inertia", &Cloth->LinearInertiaScale, 0.0f, 1.0f, "%.2f");
		bParamsChanged |= ImGui::SliderFloat("Angular Inertia", &Cloth->AngularInertiaScale, 0.0f, 1.0f, "%.2f");
		bParamsChanged |= ImGui::Checkbox("Continuous Collision", &Cloth->bEnableContinuousCollision);

		if (bParamsChanged)
		{
			Editor.ApplyClothLayerParams(LayerIndex);
		}
	}

	if (ImGui::CollapsingHeader("Bone Capsule Colliders"))
	{
		if (ImGui::Button("Reset Upper Body"))
		{
			Editor.PopulateDefaultBodyCapsules(Layer, true);
			Editor.RebuildClothLayer(LayerIndex);
		}
		ImGui::SameLine();
		if (ImGui::Button("Reset Lower Body"))
		{
			Editor.PopulateDefaultBodyCapsules(Layer, false);
			Editor.RebuildClothLayer(LayerIndex);
		}

		bool bCollidersChanged = false;
		int32 RemoveIdx = -1;
		for (int32 i = 0; i < Layer.BoneCapsules.Num(); ++i)
		{
			FClothBoneCapsule& Cap = Layer.BoneCapsules[i];
			ImGui::PushID(i);
			ImGui::Text("%s - %s", Cap.BoneA.c_str(), Cap.BoneB.c_str());
			bCollidersChanged |= ImGui::DragFloat("Radius A", &Cap.RadiusA, 0.005f, 0.01f, 0.5f, "%.3f");
			bCollidersChanged |= ImGui::DragFloat("Radius B", &Cap.RadiusB, 0.005f, 0.01f, 0.5f, "%.3f");
			if (ImGui::Button("Remove"))
			{
				RemoveIdx = i;
			}
			ImGui::Separator();
			ImGui::PopID();
		}

		if (RemoveIdx >= 0)
		{
			Layer.BoneCapsules.RemoveAt(RemoveIdx, 1);
			bCollidersChanged = true;
		}
		if (bCollidersChanged)
		{
			Editor.RebuildClothLayer(LayerIndex);
		}
	}

	if (ImGui::CollapsingHeader("Cloth Material", ImGuiTreeNodeFlags_DefaultOpen))
	{
		if (ImGui::ColorEdit4("Base Color", &Layer.ClothColor.X))
		{
			Editor.ApplyClothLayerColor(LayerIndex);
		}
	}
}

void FClothingInspectorPanel::DrawBonePoseTestSection()
{
	if (!ImGui::CollapsingHeader("Bone Pose Bend Test"))
	{
		return;
	}

	const int32 BoneIndex = Editor.GetSelectedBoneIndex();
	FTransform Transform;
	if (!Editor.GetSelectedBoneLocalTransform(Transform))
	{
		ImGui::TextDisabled("Pick a bone (e.g. thigh or calf) to bend and verify skin fit.");
		return;
	}

	ImGui::PushID(BoneIndex);
	bool bChanged = false;
	static FRotationConversionCache RotationCache;
	FRotator Euler = RotationCache.QuatToRotator(Transform.Rotation);
	if (FPropertyEditor::DrawRotator("Rotation", Euler))
	{
		Transform.Rotation = RotationCache.RotatorToQuat(Euler);
		bChanged = true;
	}
	bChanged |= FPropertyEditor::DrawVector3("Location", Transform.Location.V, 0.0f);
	ImGui::PopID();

	if (bChanged)
	{
		Editor.SetSelectedBoneLocalTransform(Transform);
	}
	if (ImGui::Button("Reset Pose"))
	{
		Editor.ResetCharacterPose();
	}
}

void FClothingInspectorPanel::DrawDebugDisplaySection()
{
	if (!ImGui::CollapsingHeader("Debug Visualization"))
	{
		return;
	}

	bool bShowWire = Editor.IsShowingRegionWireframe();
	if (ImGui::Checkbox("Show Stocking Region Wireframe", &bShowWire))
	{
		Editor.SetShowRegionWireframe(bShowWire);
	}

	bool bShowCaps = Editor.IsShowingColliders();
	if (ImGui::Checkbox("Show Cloth Capsule Colliders", &bShowCaps))
	{
		Editor.SetShowColliders(bShowCaps);
	}

	bool bShowBounds = Editor.IsShowingBounds();
	if (ImGui::Checkbox("Show Clothing Local Bounds", &bShowBounds))
	{
		Editor.SetShowBounds(bShowBounds);
	}
}

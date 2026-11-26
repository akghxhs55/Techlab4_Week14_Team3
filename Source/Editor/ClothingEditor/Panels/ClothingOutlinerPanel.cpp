#include "EnginePCH.h"
#include "Editor/ClothingEditor/Panels/ClothingOutlinerPanel.h"
#include "Editor/ClothingEditor/ClothingEditor.h"
#include <cctype>

namespace
{
	const char* GetLayerKindTag(EClothingLayerKind Kind)
	{
		switch (Kind)
		{
		case EClothingLayerKind::Stocking: return "[Stocking]";
		case EClothingLayerKind::ClothCape: return "[Cape]";
		case EClothingLayerKind::ClothSkirt: return "[Skirt]";
		case EClothingLayerKind::ClothStaticMesh: return "[MeshCloth]";
		default: return "[Clothing]";
		}
	}

	bool ContainsIgnoreCase(const FString& Text, const char* Filter)
	{
		if (!Filter || Filter[0] == '\0')
		{
			return true;
		}
		FString LowerText = Text;
		FString LowerFilter(Filter);
		for (size_t i = 0; i < LowerText.size(); ++i)
		{
			LowerText[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(LowerText[i])));
		}
		for (size_t i = 0; i < LowerFilter.size(); ++i)
		{
			LowerFilter[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(LowerFilter[i])));
		}
		return LowerText.find(LowerFilter) != FString::npos;
	}
}

FClothingOutlinerPanel::FClothingOutlinerPanel(FClothingEditor& InEditor)
	: FClothingEditorPanel(InEditor, "Clothing & Animation")
{
}

void FClothingOutlinerPanel::DrawContent()
{
	DrawLayersSection();
	ImGui::Separator();
	DrawAnimationSection();
	ImGui::Separator();
	DrawBoneTreeSection();
}

void FClothingOutlinerPanel::DrawLayersSection()
{
	if (!ImGui::CollapsingHeader("Clothing Layers", ImGuiTreeNodeFlags_DefaultOpen))
	{
		return;
	}

	if (ImGui::Button("+ Stocking"))
	{
		Editor.AddStockingLayer("Stocking", true);
	}
	ImGui::SameLine();
	if (ImGui::Button("+ Cape"))
	{
		Editor.AddCapeClothLayer("Cape");
	}
	ImGui::SameLine();
	if (ImGui::Button("+ Skirt"))
	{
		Editor.AddSkirtClothLayer("Skirt");
	}
	ImGui::SameLine();
	if (ImGui::Button("+ Mesh"))
	{
		Editor.AddStaticMeshClothLayer("MeshCloth");
	}

	int32 IndexToRemove = -1;
	for (int32 i = 0; i < Editor.GetLayerCount(); ++i)
	{
		FClothingLayer* Layer = Editor.GetLayer(i);
		if (!Layer)
		{
			continue;
		}

		ImGui::PushID(i);
		bool bVisible = Layer->bVisible;
		if (ImGui::Checkbox("##Visible", &bVisible))
		{
			Editor.SetLayerVisible(i, bVisible);
		}
		ImGui::SameLine();

		const bool bSelected = (Editor.GetSelectedLayerIndex() == i);
		const FString Label = FString(GetLayerKindTag(Layer->Kind)) + " " + Layer->Name;
		if (ImGui::Selectable(Label.c_str(), bSelected, ImGuiSelectableFlags_AllowOverlap))
		{
			Editor.SelectLayer(i);
		}

		if (ImGui::BeginPopupContextItem("##LayerCtx"))
		{
			if (ImGui::MenuItem("Remove Layer"))
			{
				IndexToRemove = i;
			}
			ImGui::EndPopup();
		}
		ImGui::PopID();
	}

	if (IndexToRemove >= 0)
	{
		Editor.RemoveLayer(IndexToRemove);
	}
	else if (Editor.GetSelectedLayerIndex() >= 0 && ImGui::Button("Remove Selected Layer"))
	{
		Editor.RemoveLayer(Editor.GetSelectedLayerIndex());
	}
}

void FClothingOutlinerPanel::DrawAnimationSection()
{
	if (!ImGui::CollapsingHeader("Animation Preview", ImGuiTreeNodeFlags_DefaultOpen))
	{
		return;
	}

	if (ImGui::Button(Editor.IsAnimationPlaying() ? "Pause" : "Play"))
	{
		Editor.ToggleAnimationPlayback();
	}
	ImGui::SameLine();
	if (ImGui::Button("Stop / Bind Pose"))
	{
		Editor.StopAnimation();
	}
	ImGui::SameLine();
	bool bLoop = Editor.IsAnimationLooping();
	if (ImGui::Checkbox("Loop", &bLoop))
	{
		Editor.SetAnimationLooping(bLoop);
	}

	if (!Editor.GetCurrentAnimationPath().empty())
	{
		ImGui::TextDisabled("Active: %s", Editor.GetCurrentAnimationPath().c_str());
	}

	if (ImGui::BeginChild("##ClothingAnimBrowser", ImVec2(0.0f, 180.0f), true))
	{
		FString OpenedAnimPath;
		if (AnimBrowser.Draw(Editor.GetMeshAssetPath(), Editor.GetCurrentAnimationPath(), false, OpenedAnimPath))
		{
			Editor.PlayAnimationByPath(OpenedAnimPath);
		}
	}
	ImGui::EndChild();
}

void FClothingOutlinerPanel::DrawBoneTreeSection()
{
	if (!ImGui::CollapsingHeader("Skeleton Bones"))
	{
		return;
	}

	ImGui::InputTextWithHint("##BoneFilter", "Filter bones...", BoneFilterBuffer, sizeof(BoneFilterBuffer));
	if (ImGui::BeginChild("##ClothingBoneTree", ImVec2(0.0f, 200.0f), true))
	{
		const TArray<int32>& Roots = Editor.GetRootBoneIndices();
		for (int32 i = 0; i < Roots.Num(); ++i)
		{
			DrawBoneNodeRecursive(Roots[i]);
		}
	}
	ImGui::EndChild();
}

void FClothingOutlinerPanel::DrawBoneNodeRecursive(int32 BoneIndex)
{
	const TArray<FEditorBoneItem>& Items = Editor.GetBoneItems();
	if (BoneIndex < 0 || BoneIndex >= Items.Num())
	{
		return;
	}

	const FEditorBoneItem& Item = Items[BoneIndex];
	const bool bHasFilter = BoneFilterBuffer[0] != '\0';
	const bool bMatches = ContainsIgnoreCase(Item.Name, BoneFilterBuffer);

	if (bHasFilter)
	{
		if (bMatches)
		{
			const bool bSelected = (Editor.GetSelectedBoneIndex() == BoneIndex);
			if (ImGui::Selectable(Item.Name.c_str(), bSelected))
			{
				Editor.SelectBone(BoneIndex);
			}
		}
		for (int32 i = 0; i < Item.ChildBoneIndices.Num(); ++i)
		{
			DrawBoneNodeRecursive(Item.ChildBoneIndices[i]);
		}
		return;
	}

	ImGuiTreeNodeFlags Flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
	if (Item.ChildBoneIndices.IsEmpty())
	{
		Flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
	}
	if (Editor.GetSelectedBoneIndex() == BoneIndex)
	{
		Flags |= ImGuiTreeNodeFlags_Selected;
	}

	ImGui::PushID(BoneIndex);
	const bool bOpen = ImGui::TreeNodeEx(Item.Name.c_str(), Flags);
	if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
	{
		Editor.SelectBone(BoneIndex);
	}
	if (bOpen && !Item.ChildBoneIndices.IsEmpty())
	{
		for (int32 i = 0; i < Item.ChildBoneIndices.Num(); ++i)
		{
			DrawBoneNodeRecursive(Item.ChildBoneIndices[i]);
		}
		ImGui::TreePop();
	}
	ImGui::PopID();
}

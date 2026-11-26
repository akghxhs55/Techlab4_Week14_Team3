#include "EnginePCH.h"
#include "Editor/WorldSettings/WorldSettingsPanel.h"

#include "Engine/World.h"
#include "GameFramework/GameMode.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/GameHUD.h"
#include "UObject/UObjectHash.h"
#include "Editor/EditorUI/EditorStyle.h"

#include <algorithm>
#include <cctype>

namespace
{
	FString FormatClassDisplayName(const UClass* Class)
	{
		if (!Class)
		{
			return "None";
		}
		FString Name = Class->Name;
		if (Name.size() > 1 && (Name[0] == 'A' || Name[0] == 'U') && std::isupper(static_cast<unsigned char>(Name[1])))
		{
			Name.erase(0, 1);
		}
		return Name;
	}

	TArray<UClass*> GatherInstantiableClasses(UClass* BaseClass)
	{
		TArray<UClass*> Result;
		if (!BaseClass)
		{
			return Result;
		}
		if (BaseClass->Constructor)
		{
			Result.Add(BaseClass);
		}
		TArray<UClass*> Derived;
		GetDerivedClasses(BaseClass, Derived, true);
		for (UClass* Class : Derived)
		{
			if (Class && Class->Constructor && Result.Find(Class) == INDEX_NONE)
			{
				Result.Add(Class);
			}
		}
		std::sort(Result.begin(), Result.end(), [](const UClass* A, const UClass* B)
			{
				return FormatClassDisplayName(A) < FormatClassDisplayName(B);
			});
		return Result;
	}

	bool DrawClassOverrideCombo(const char* Id, UClass* BaseClass, const FString& OverrideName, UClass* EffectiveClass, UClass*& OutSelected, bool& bOutClearOverride)
	{
		OutSelected = nullptr;
		bOutClearOverride = false;

		const bool bHasOverride = !OverrideName.empty();
		const FString Preview = bHasOverride
			? FormatClassDisplayName(EffectiveClass)
			: ("None (" + FormatClassDisplayName(EffectiveClass) + ")");

		ImGui::SetNextItemWidth(-FLT_MIN);
		if (!ImGui::BeginCombo(Id, Preview.c_str()))
		{
			return false;
		}

		bool bChanged = false;
		const FString ClearLabel = "None (Use GameMode Default: " + FormatClassDisplayName(EffectiveClass) + ")";
		if (ImGui::Selectable(ClearLabel.c_str(), !bHasOverride))
		{
			bOutClearOverride = true;
			bChanged = true;
		}

		const TArray<UClass*> Candidates = GatherInstantiableClasses(BaseClass);
		for (UClass* Candidate : Candidates)
		{
			const bool bSelected = bHasOverride && Candidate && Candidate->Name == OverrideName;
			const FString Label = FormatClassDisplayName(Candidate);
			ImGui::PushID(Candidate);
			if (ImGui::Selectable(Label.c_str(), bSelected))
			{
				OutSelected = Candidate;
				bChanged = true;
			}
			if (ImGui::IsItemHovered())
			{
				ImGui::SetTooltip("%s", Candidate->Name.c_str());
			}
			if (bSelected)
			{
				ImGui::SetItemDefaultFocus();
			}
			ImGui::PopID();
		}

		ImGui::EndCombo();
		return bChanged;
	}
}

bool FWorldSettingsPanel::Init()
{
	return true;
}

void FWorldSettingsPanel::Tick(float /*DeltaTime*/)
{
}

void FWorldSettingsPanel::OnRender()
{
	if (!ImGui::Begin("World Settings", GetOpenFlag()))
	{
		bFocused = false;
		ImGui::End();
		return;
	}
	bFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

	if (!World)
	{
		ImGui::TextDisabled("No active world.");
		ImGui::End();
		return;
	}

	const bool bIsGameWorld = World->IsGameWorld();
	ImGui::BeginDisabled(bIsGameWorld);

	if (ImGui::CollapsingHeader("Game Mode", ImGuiTreeNodeFlags_DefaultOpen))
	{
		if (ImGui::BeginTable("##WorldGameModeTable", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX))
		{
			ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthStretch, 0.42f);
			ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.58f);

			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted("GameMode Override");
			ImGui::TableSetColumnIndex(1);

			UClass* SelectedMode = nullptr;
			bool bClearMode = false;
			if (DrawClassOverrideCombo("##GameModeOverride", AGameMode::StaticClass(),
				World->GetGameModeClassName(), World->GetGameModeClass(), SelectedMode, bClearMode))
			{
				World->SetGameModeClass(bClearMode ? nullptr : SelectedMode);
			}

			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted("Default Pawn Class");
			ImGui::TableSetColumnIndex(1);

			UClass* SelectedPawn = nullptr;
			bool bClearPawn = false;
			if (DrawClassOverrideCombo("##DefaultPawnOverride", APawn::StaticClass(),
				World->GetPlayerPawnClassName(), World->GetDefaultPawnClass(), SelectedPawn, bClearPawn))
			{
				World->SetPlayerPawnClass(bClearPawn ? nullptr : SelectedPawn);
			}

			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted("Player Controller Class");
			ImGui::TableSetColumnIndex(1);

			UClass* SelectedPC = nullptr;
			bool bClearPC = false;
			if (DrawClassOverrideCombo("##PlayerControllerOverride", APlayerController::StaticClass(),
				World->GetPlayerControllerClassName(), World->GetPlayerControllerClass(), SelectedPC, bClearPC))
			{
				World->SetPlayerControllerClass(bClearPC ? nullptr : SelectedPC);
			}

			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted("HUD Class");
			ImGui::TableSetColumnIndex(1);

			UClass* SelectedHUD = nullptr;
			bool bClearHUD = false;
			if (DrawClassOverrideCombo("##HUDOverride", AGameHUD::StaticClass(),
				World->GetHUDClassName(), World->GetHUDClass(), SelectedHUD, bClearHUD))
			{
				World->SetHUDClass(bClearHUD ? nullptr : SelectedHUD);
			}

			ImGui::EndTable();
		}
	}

	if (ImGui::CollapsingHeader("World", ImGuiTreeNodeFlags_DefaultOpen))
	{
		if (ImGui::BeginTable("##WorldTimeTable", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX))
		{
			ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthStretch, 0.42f);
			ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.58f);

			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted("Global Time Dilation");
			ImGui::TableSetColumnIndex(1);
			float GlobalDilation = World->GetGlobalTimeDilation();
			const float ResetWidth = EditorStyle::Scale(48.0f);
			const float InputWidth = (std::max)(EditorStyle::Scale(40.0f),
				ImGui::GetContentRegionAvail().x - ResetWidth - ImGui::GetStyle().ItemSpacing.x);
			ImGui::SetNextItemWidth(InputWidth);
			if (ImGui::DragFloat("##GlobalTimeDilation", &GlobalDilation, 0.01f, 0.0001f, 20.0f, "%.3f"))
			{
				World->SetGlobalTimeDilation(GlobalDilation);
			}
			ImGui::SameLine();
			if (ImGui::Button("Reset", ImVec2(ResetWidth, 0.0f)))
			{
				World->SetGlobalTimeDilation(1.0f);
			}

			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted("Cinematic Time Dilation");
			ImGui::TableSetColumnIndex(1);
			float CinematicDilation = World->GetCinematicTimeDilation();
			ImGui::SetNextItemWidth(-FLT_MIN);
			if (ImGui::DragFloat("##CinematicTimeDilation", &CinematicDilation, 0.01f, 0.0001f, 20.0f, "%.3f"))
			{
				World->SetCinematicTimeDilation(FMath::Clamp(CinematicDilation, 0.0001f, 20.0f));
			}

			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted("Effective Time Dilation");
			ImGui::TableSetColumnIndex(1);
			ImGui::AlignTextToFramePadding();
			ImGui::Text("%.3f", World->GetEffectiveTimeDilation());

			ImGui::EndTable();
		}
	}

	ImGui::EndDisabled();

	ImGui::Spacing();
	if (bIsGameWorld)
	{
		ImGui::TextDisabled("World Settings are read-only during Play In Editor.");
	}
	else
	{
		ImGui::TextDisabled("World Settings are saved with the current scene (Ctrl+S).");
	}

	ImGui::End();
}

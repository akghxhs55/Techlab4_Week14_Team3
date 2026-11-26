#include "EnginePCH.h"
#include "Editor/KurisuEd/EditorFileUtils.h"

#include "Core/EngineLog.h"
#include <commdlg.h>
#include <optional>
#include <imgui.h>
#include "Serialization/JsonArchive.h"
#include "Core/Misc/Paths.h"
#include "Editor/Asset/FbxImporter/FbxImporter.h"
 
FString FEditorFileUtils::CurrentScenePath = "";

namespace
{
	constexpr const char* FbxImportOptionsPopup = "FBX Import Options";

	struct FPendingFbxImport
	{
		FString Path;
		FFbxImportOptions Options;
		bool bOpenRequested = true;
	};
	std::optional<FPendingFbxImport> PendingFbxImport;
	bool bLastCreatePhysicsAsset = true;

	void RunFbxImport(const FString& Path, const FFbxImportOptions& Options)
	{
		FFbxImportResult Result;
		FString Error;
		if (!FFbxImporter::ImportToAssets(Path, Options, Result, Error))
		{
			KRS_LOG(Error, "FBX Import failed: {}", Error);
			MessageBoxW(GetActiveWindow(), L"Failed to import FBX. Check the Output Log for errors.", L"FBX Import", MB_OK | MB_ICONERROR);
		}
	}
}

void FEditorFileUtils::ImportFbx(EFbxImportMode Mode)
{
	if (PendingFbxImport) return;

	wchar_t FilePath[32768] = {};
	OPENFILENAMEW Dialog = {};
	Dialog.lStructSize = sizeof(Dialog);
	Dialog.hwndOwner = GetActiveWindow();
	Dialog.lpstrFile = FilePath;
	Dialog.nMaxFile = static_cast<DWORD>(std::size(FilePath));
	Dialog.lpstrFilter = L"FBX Files (*.fbx)\0*.fbx\0";
	Dialog.lpstrTitle = Mode == EFbxImportMode::Static ? L"Import Static Mesh" : L"Import Skeletal Mesh";
	Dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
	if (!GetOpenFileNameW(&Dialog))
	{
		const DWORD Error = CommDlgExtendedError();
		if (Error != 0) KRS_LOG(Error, "FBX file dialog failed: {}", Error);
		return;
	}

	// 파일 선택의 UTF-16 경로를 Importer가 받는 UTF-8로 변환한다.
	const auto Utf8 = std::filesystem::path(FilePath).generic_u8string();
	FFbxImportOptions Options;
	Options.Mode = Mode;
	Options.bCreatePhysicsAsset = bLastCreatePhysicsAsset;
	const FString Path(Utf8.begin(), Utf8.end());

	if (Mode != EFbxImportMode::Skeletal)
	{
		RunFbxImport(Path, Options);
		return;
	}
	PendingFbxImport = FPendingFbxImport{ Path, Options };
}

void FEditorFileUtils::DrawFbxImportOptions()
{
	if (!PendingFbxImport) return;
	if (PendingFbxImport->bOpenRequested)
	{
		ImGui::OpenPopup(FbxImportOptionsPopup);
		PendingFbxImport->bOpenRequested = false;
	}

	ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
	if (!ImGui::BeginPopupModal(FbxImportOptionsPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		if (!ImGui::IsPopupOpen(FbxImportOptionsPopup)) PendingFbxImport.reset();
		return;
	}

	const auto FileName = std::filesystem::path(FPaths::FromUtf8(PendingFbxImport->Path)).filename().u8string();
	ImGui::Text("File: %s", reinterpret_cast<const char*>(FileName.c_str()));
	ImGui::Separator();

	ImGui::Checkbox("Create Physics Asset", &PendingFbxImport->Options.bCreatePhysicsAsset);
	ImGui::TextDisabled("Generates bodies and constraints only when no .physics.json exists.");
	ImGui::Spacing();

	bool bImport = ImGui::Button("Import", ImVec2(120.0f, 0.0f));
	ImGui::SameLine();
	const bool bCancel = ImGui::Button("Cancel", ImVec2(120.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
	if (bCancel) bImport = false;

	std::optional<FPendingFbxImport> Accepted;
	if (bImport || bCancel)
	{
		if (bImport)
		{
			bLastCreatePhysicsAsset = PendingFbxImport->Options.bCreatePhysicsAsset;
			Accepted = std::move(PendingFbxImport);
		}
		PendingFbxImport.reset();
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndPopup();

	if (Accepted) RunFbxImport(Accepted->Path, Accepted->Options);
}

bool FEditorFileUtils::NewScene(UWorld* World)
{
	if (!World)
		return false;

	World->ClearWorld();

	CurrentScenePath.clear();

	return true;
}

bool FEditorFileUtils::SaveScene(UWorld* World)
{
	if (!World)
		return false;

	if (CurrentScenePath.empty())
	{
		return SaveSceneAs(World);
	}

	if (!FJsonArchive::SaveWorld(World, CurrentScenePath))
	{
		KRS_LOG(Error, "Failed to save scene: {}", CurrentScenePath);

		MessageBoxW(
			GetActiveWindow(),
			L"Failed to save the scene.\nCheck the path and write permission.",
			L"Save Failed",
			MB_OK | MB_ICONERROR
		);
		return false;
	}
	KRS_LOG(Info, "Save Scene : {}", CurrentScenePath);

	return true;
}

bool FEditorFileUtils::SaveSceneAs(UWorld* World)
{
	if (!World)
		return false;

	FString FilePath = OpenSaveSceneDialog();

	if (FilePath.empty())
		return false;

	FilePath = std::filesystem::absolute(FilePath).lexically_normal().string();

	if (!FJsonArchive::SaveWorld(World, FilePath))
	{
		KRS_LOG(Error, "Failed to save scene: {}", FilePath);

		MessageBoxW(
			GetActiveWindow(),
			L"Failed to save the scene.\nCheck the path and write permission.",
			L"Save Failed",
			MB_OK | MB_ICONERROR
		);

		return false;
	}

	CurrentScenePath = FilePath;

	KRS_LOG(Info, "Save Scene As : {}", CurrentScenePath);

	return true;
}

bool FEditorFileUtils::LoadScene(UWorld* World)
{
	if (!World)
		return false;

	const FString FilePath = OpenLoadSceneDialog();

	// 파일 선택 취소
	if (FilePath.empty())
		return false;

	return LoadScene(World, FilePath);
}

bool FEditorFileUtils::LoadScene(UWorld* World, const FString& InFilePath)
{
	if (!World || InFilePath.empty())
		return false;

	const FString FilePath = std::filesystem::absolute(InFilePath).lexically_normal().string();

	if (!FJsonArchive::LoadWorld(World, FilePath))
		return false;

	// 이제 이 파일이 현재 Scene
	CurrentScenePath = FilePath;

	KRS_LOG(Info, "Load Scene : {}", CurrentScenePath);

	return true;
}

FString FEditorFileUtils::OpenSaveSceneDialog()
{
	char FilePath[MAX_PATH] = {};

	OPENFILENAMEA ofn = {};
	ofn.lStructSize = sizeof(OPENFILENAMEA);
	ofn.hwndOwner = nullptr;
	ofn.lpstrFile = FilePath;
	ofn.nMaxFile = MAX_PATH;

	ofn.lpstrFilter = "Scene Files (*.scene)\0*.scene\0";
	ofn.lpstrDefExt = "scene";
	const FString SceneDirectory = FPaths::ToUtf8(FPaths::ProjectScenesDir());
	ofn.lpstrInitialDir = SceneDirectory.c_str();

	ofn.Flags =
		OFN_PATHMUSTEXIST |
		OFN_OVERWRITEPROMPT |
		OFN_NOCHANGEDIR;

	if (GetSaveFileNameA(&ofn))
	{
		return FString(FilePath);
	}

	return FString();
}

FString FEditorFileUtils::OpenLoadSceneDialog()
{
	char FilePath[MAX_PATH] = {};

	OPENFILENAMEA ofn = {};
	ofn.lStructSize = sizeof(OPENFILENAMEA);
	ofn.hwndOwner = nullptr;

	ofn.lpstrFile = FilePath;
	ofn.nMaxFile = MAX_PATH;

	ofn.lpstrFilter =
		"Scene Files (*.scene)\0*.scene\0"
		"All Files (*.*)\0*.*\0";

	ofn.lpstrDefExt = "scene";
	ofn.lpstrInitialDir = "Scene/";

	ofn.Flags =
		OFN_FILEMUSTEXIST |
		OFN_PATHMUSTEXIST |
		OFN_NOCHANGEDIR;

	if (GetOpenFileNameA(&ofn))
	{
		return FString(FilePath);
	}

	return FString();
}



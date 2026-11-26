#include "EnginePCH.h"
#include "Editor/AssetEditor/AssetEditorTab.h"
#include "Editor/AssetEditor/AssetEditor.h"
#include <filesystem>

FAssetEditorTab::FAssetEditorTab(FAssetEditor& InEditor) : Editor(InEditor)
{
	// ### 뒤 ID는 처음 연 문서로 고정합니다. 에셋 경로가 같아도 전용 에디터는 별도 탭을 가집니다.
	TabId = FString("###AssetEditor:") + Editor.GetDocumentKey();
}

const char* FAssetEditorTab::GetTabName() const
{
	// 보이는 이름은 지금 경로로 만든다. 경로가 바뀐 때만 다시 만든다.
	if (NamedPath != Editor.GetAssetPath())
	{
		NamedPath = Editor.GetAssetPath();
		const FString Filename = std::filesystem::path(NamedPath).filename().string();
		TabName = Filename + " (" + Editor.GetEditorTypeName() + ")" + TabId;
	}
	return TabName.c_str();
}

bool FAssetEditorTab::ConfirmClose()
{
	return Editor.ConfirmClose();
}

bool FAssetEditorTab::HasUnsavedChanges() const
{
	return Editor.IsDirty();
}

void FAssetEditorTab::OnCloseRequested()
{
	Editor.Close();
}

void FAssetEditorTab::DrawMenuBar()
{
	// 기본 메뉴 바. 레벨 에디터와 같이 File 메뉴부터 둔다.

	if (ImGui::BeginMenu("File"))
	{
		if (ImGui::MenuItem("Close Editor"))
		{
			Editor.Close();
		}

		ImGui::EndMenu();
	}
}

void FAssetEditorTab::OnRender(bool bActive)
{
}

#pragma once

#include "Editor/EditorUI/EditorPanel.h"

class UWorld;

class FWorldSettingsPanel : public IEditorPanel
{
public:
	FWorldSettingsPanel() = default;
	~FWorldSettingsPanel() = default;

	bool Init() override;
	void Tick(float DeltaTime) override;
	void OnRender() override;
	const char* GetPanelName() const override { return "World Settings"; }
	bool IsFocused() const override { return bFocused; }

	void SetWorld(UWorld* InWorld) { World = InWorld; }

private:
	UWorld* World = nullptr;
	bool bFocused = false;
};

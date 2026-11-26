#pragma once

#include "Editor/SkeletalPreviewEditor/SkeletalPreviewEditor.h"

class UBlendSpace1D;
class UBlendSpace2D;

class FBlendSpaceEditor : public FSkeletalPreviewEditor
{
public:
    FBlendSpaceEditor(UEditorEngine& InEditorEngine, FEditorManager& InEditorManager,
        const FString& InAssetPath, UBlendSpace1D* InBlendSpace, UBlendSpace2D* InBlendSpace2D = nullptr);

    const char* GetEditorTypeName() const override { return "BlendSpace"; }
    bool Save();
    const FString& GetMessage() const { return Message; }
    UBlendSpace1D* GetBlendSpace() const { return BlendSpace; }
    UBlendSpace2D* GetBlendSpace2D() const { return BlendSpace2D; }
    bool Is2D() const { return BlendSpace2D != nullptr; }

protected:
    bool InitializeResources() override;
    void ShutdownResources() override;
    TUniquePtr<IEditorTab> CreateTab() override;

private:
    UBlendSpace1D* BlendSpace = nullptr;
    UBlendSpace2D* BlendSpace2D = nullptr;
    FString Message;
};
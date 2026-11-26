#include "EnginePCH.h"

#include "BlendSpaceEditor.h"
#include "BlendSpaceEditorTab.h"
#include "Animation/BlendSpace1D.h"
#include "Animation/BlendSpace2D.h"
#include "Animation/BlendSpaceFile.h"
#include "Asset/AssetManager.h"
#include "Asset/AssetUtils.h"
#include "Components/SkeletalMeshComponent.h"
#include "Render/SkeletalMesh.h"
#include "UObject/Casts.h"

FBlendSpaceEditor::FBlendSpaceEditor(UEditorEngine& InEditorEngine, FEditorManager& InEditorManager,
    const FString& InAssetPath, UBlendSpace1D* InBlendSpace, UBlendSpace2D* InBlendSpace2D)
    : FSkeletalPreviewEditor(InEditorEngine, InEditorManager, InAssetPath)
    , BlendSpace(InBlendSpace)
    , BlendSpace2D(InBlendSpace2D)
{
}

bool FBlendSpaceEditor::InitializeResources()
{
    if (!BlendSpace && !BlendSpace2D)
    {
        return false;
    }

    const FString& PreviewPath = BlendSpace2D ? BlendSpace2D->GetPreviewMeshPath() : BlendSpace->GetPreviewMeshPath();
    if (PreviewPath.empty())
    {
        KRS_LOG(Warning, "BlendSpace has no preview mesh: {}", GetAssetPath());
        return false;
    }

    const FString FilePath = FAssetUtils::ResolveAssetFilePath(PreviewPath);
    FString Error;
    Mesh = Cast<USkeletalMesh>(UAssetManager::LoadFbxMesh(FilePath.empty() ? PreviewPath : FilePath, Error));

    if (!Mesh)
    {
        KRS_LOG(Warning, "BlendSpace preview mesh failed: {} ({})", PreviewPath, Error);
        return false;
    }

    if (!InitializePreview())
    {
        return false;
    }

    UAnimationAsset* AnimAsset = BlendSpace2D ? static_cast<UAnimationAsset*>(BlendSpace2D) : static_cast<UAnimationAsset*>(BlendSpace);
    MeshComponent->SetAnimation(AnimAsset);
    MeshComponent->BlendSpaceInput = 0.0f;
    MeshComponent->BlendSpaceInputY = 0.0f;
    MeshComponent->RefreshAnimationPose();
    return true;
}

void FBlendSpaceEditor::ShutdownResources()
{
    if (MeshComponent)
    {
        MeshComponent->Stop();
    }
    FSkeletalPreviewEditor::ShutdownResources();
    BlendSpace = nullptr;
    BlendSpace2D = nullptr;
}

TUniquePtr<IEditorTab> FBlendSpaceEditor::CreateTab()
{
    return MakeUnique<FBlendSpaceEditorTab>(*this);
}

bool FBlendSpaceEditor::Save()
{
    if (!BlendSpace && !BlendSpace2D)
    {
        return false;
    }

    const FString FilePath = FAssetUtils::ResolveAssetFilePath(GetAssetPath());
    const std::filesystem::path TargetPath = FilePath.empty() ? GetAssetPath() : FilePath;
    if (BlendSpace2D)
    {
        if (!BlendSpaceFile::Write2D(TargetPath, *BlendSpace2D, Message))
            return false;
        Message = "BlendSpace 2D saved.";
        return true;
    }

    if (!BlendSpaceFile::Write(TargetPath, *BlendSpace, Message))
    {
        return false;
    }

    Message = "BlendSpace saved.";
    return true;
}
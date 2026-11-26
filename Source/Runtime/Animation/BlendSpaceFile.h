#pragma once

#include "Core/EngineString.h"

#include <filesystem>

class UBlendSpace1D;
class UBlendSpace2D;

namespace BlendSpaceFile
{
    bool Read(const std::filesystem::path& Path, UBlendSpace1D& BlendSpace, FString& OutError);
    bool Write(const std::filesystem::path& Path, UBlendSpace1D& BlendSpace, FString& OutError);
    bool Read2D(const std::filesystem::path& Path, UBlendSpace2D& BlendSpace, FString& OutError);
    bool Write2D(const std::filesystem::path& Path, UBlendSpace2D& BlendSpace, FString& OutError);
    bool IsBlendSpace2DFile(const std::filesystem::path& Path);
    bool IsBlendSpacePath(const FString& Path);
}
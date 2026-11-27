#pragma once

#include "Core/EngineString.h"
#include "Containers/Array.h"
#include <filesystem>

class ALandscapeProxy;

// 16비트 높이맵(.r16/.raw) 파일을 읽어 Landscape에 임포트
// 파일 형식: 헤더 없음, 리틀 엔디언 uint16, 행 우선(행 = Y), N x N 정사각형
namespace LandscapeHeightmapImport
{
	bool IsHeightmapPath(const FString& Path);

	bool LoadR16(const std::filesystem::path& Path, TArray<uint16>& OutHeightData, int32& OutSize, FString& OutError);

	// InSize x InSize 높이맵을 (k * ComponentSizeQuads + 1) 크기로 맞춘다. 범위 밖은 Clamp
	void PadToComponentSize(const TArray<uint16>& InHeightData, int32 InSize, int32 ComponentSizeQuads, TArray<uint16>& OutHeightData, int32& OutSize);

	bool ImportFromFile(ALandscapeProxy& Landscape, const std::filesystem::path& Path, FString& OutError);
}

#include "EnginePCH.h"
#include "LandscapeHeightmapImport.h"

#include "LandscapeProxy.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>

bool LandscapeHeightmapImport::IsHeightmapPath(const FString& Path)
{
	FString Extension = std::filesystem::path(Path).extension().string();

	std::transform(Extension.begin(), Extension.end(), Extension.begin(),
		[](unsigned char Character) { return static_cast<char>(std::tolower(Character)); });

	return Extension == ".r16" || Extension == ".raw";
}

bool LandscapeHeightmapImport::LoadR16(const std::filesystem::path& Path, TArray<uint16>& OutHeightData, int32& OutSize, FString& OutError)
{
	OutHeightData.Empty();
	OutSize = 0;

	std::error_code SizeError;
	const uintmax_t FileSize = std::filesystem::file_size(Path, SizeError);
	if (SizeError)
	{
		OutError = "Heightmap: cannot read file size";
		return false;
	}

	if (FileSize % 2 != 0)
	{
		OutError = std::format("Heightmap: file size {} is not a multiple of 2 bytes", FileSize);
		return false;
	}

	const uint64 SampleCount = FileSize / 2;
	const uint64 Size = static_cast<uint64>(std::llround(std::sqrt(static_cast<double>(SampleCount))));
	if (Size * Size != SampleCount)
	{
		OutError = std::format("Heightmap: {} samples is not a square (expected 2 * N^2 bytes)", SampleCount);
		return false;
	}
	if (Size < 2)
	{
		OutError = "Heightmap: size must be at least 2 x 2";
		return false;
	}
	if (Size > static_cast<uint64>(INT32_MAX) || SampleCount > static_cast<uint64>(INT32_MAX))
	{
		OutError = "Heightmap: file is too large";
		return false;
	}

	std::ifstream File(Path, std::ios::binary);
	if (!File)
	{
		OutError = "Heightmap: cannot open file";
		return false;
	}

	// x86/x64는 리틀 엔디언이므로 그대로 읽는다.
	OutHeightData.SetNum(static_cast<int32>(SampleCount));
	File.read(reinterpret_cast<char*>(OutHeightData.GetData()), static_cast<std::streamsize>(FileSize));
	if (File.gcount() != static_cast<std::streamsize>(FileSize))
	{
		OutHeightData.Empty();
		OutError = "Heightmap: failed to read file";
		return false;
	}

	OutSize = static_cast<int32>(Size);
	return true;
}

void LandscapeHeightmapImport::PadToComponentSize(const TArray<uint16>& InHeightData, int32 InSize, int32 ComponentSizeQuads, TArray<uint16>& OutHeightData, int32& OutSize)
{
	const int32 k = (InSize - 1 + ComponentSizeQuads - 1) / ComponentSizeQuads;
	OutSize = k * ComponentSizeQuads + 1;
	
	OutHeightData.SetNum(OutSize * OutSize);
	for (int32 y = 0; y < OutSize; y++)
	{
		for (int32 x = 0; x < OutSize; x++)
		{
			const int32 InY = std::min(y, InSize - 1);
			const int32 InX = std::min(x, InSize - 1);
			OutHeightData[y * OutSize + x] = InHeightData[InY * InSize + InX];
		}
	}
}

bool LandscapeHeightmapImport::ImportFromFile(ALandscapeProxy& Landscape, const std::filesystem::path& Path, FString& OutError)
{
	const int32 ComponentSizeQuads = Landscape.ComponentSizeQuads;
	if (ComponentSizeQuads <= 0)
	{
		OutError = "Heightmap: invalid ComponentSizeQuads";
		return false;
	}

	TArray<uint16> SourceData;
	int32 SourceSize = 0;
	if (!LoadR16(Path, SourceData, SourceSize, OutError))
	{
		return false;
	}

	TArray<uint16> PaddedData;
	int32 PaddedSize = 0;
	PadToComponentSize(SourceData, SourceSize, ComponentSizeQuads, PaddedData, PaddedSize);

	// Import의 assert를 타기 전에 패딩 결과를 검증한다.
	if (PaddedSize < 2 || (PaddedSize - 1) % ComponentSizeQuads != 0 || PaddedData.Num() != PaddedSize * PaddedSize)
	{
		OutError = std::format("Heightmap: size {} does not fit component size {} (need k * {} + 1)",
			PaddedSize, ComponentSizeQuads, ComponentSizeQuads);
		return false;
	}

	Landscape.Import(0, 0, PaddedSize - 1, PaddedSize - 1, PaddedData);
	return true;
}

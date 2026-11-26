#pragma once

#include "Asset/AssetUtils.h"
#include "Asset/FbxImporter/FbxMeshBake.h"
#include "Core/Misc/Paths.h"
#include "Physics/PhysicsAsset.h"
#include "Serialization/JsonArchive.h"
#include "UObject/UObjectGlobals.h"
#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <Windows.h>

namespace PhysicsAssetFile
{
	inline bool IsPhysicsAssetPath(const FString& Path)
	{
		FString Lower = Path;
		std::transform(Lower.begin(), Lower.end(), Lower.begin(),
			[](unsigned char Ch) { return static_cast<char>(std::tolower(Ch)); });
		return Lower.ends_with(".physics.json");
	}

	// UObject/메시를 로드하지 않고 참조 경로만 읽는다. 기존 sidecar 파일도 지원한다.
	inline FString ResolvePreviewMeshPath(const FString& AssetPath, FString& OutError)
	{
		OutError.clear();
		const FString FilePath = FAssetUtils::ResolveAssetFilePath(AssetPath);
		std::ifstream File(FPaths::FromUtf8(FilePath), std::ios::binary);
		const nlohmann::json Root = File ? nlohmann::json::parse(File, nullptr, false) : nlohmann::json();
		if (!Root.is_object() || !Root.contains("Class") || Root["Class"] != "UPhysicsAsset"
			|| !Root.contains("Object") || !Root["Object"].is_object())
		{
			OutError = "Cannot read the Physics Asset file: " + AssetPath;
			return {};
		}
		const auto& Object = Root["Object"];
		FString MeshPath;
		if (Object.contains("PreviewMeshPath") && Object["PreviewMeshPath"].is_string())
			MeshPath = Object["PreviewMeshPath"].get<FString>();
		if (MeshPath.empty() && IsPhysicsAssetPath(FilePath))
		{
			const FString LegacyMeshPath = FilePath.substr(0, FilePath.size() - sizeof(".physics.json") + 1);
			if (FFbxMeshBake::IsBakedPath(LegacyMeshPath)) MeshPath = LegacyMeshPath;
		}
		if (MeshPath.empty() || !FFbxMeshBake::IsBakedPath(MeshPath))
		{
			OutError = "The Physics Asset does not reference a skeletal mesh: " + AssetPath;
			return {};
		}
		const FString Resolved = FAssetUtils::ResolveAssetFilePath(MeshPath);
		if (Resolved.empty()) OutError = "Cannot resolve the Physics Asset's skeletal mesh path: " + MeshPath;
		return Resolved;
	}

	inline bool CreateNew(const std::filesystem::path& Target, const FString& MeshPath, FString& OutError)
	{
		OutError.clear();
		TUniquePtr<UPhysicsAsset> Asset(NewObject<UPhysicsAsset>());
		if (!Asset)
		{
			OutError = "Could not allocate the new Physics Asset.";
			return false;
		}
		Asset->PreviewMeshPath = FAssetUtils::GetAssetKey(MeshPath);
		TArray<uint8> Bytes;
		if (Asset->PreviewMeshPath.empty() || !FJsonArchive::SaveObjectToBytes(Asset.get(), Bytes))
		{
			OutError = "Could not serialize the new Physics Asset.";
			return false;
		}
		// 같은 이름의 파일을 생성 시점에도 보호한다.
		const HANDLE File = CreateFileW(Target.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (File == INVALID_HANDLE_VALUE)
		{
			OutError = "Cannot create the Physics Asset. Check its name and write access.";
			return false;
		}
		DWORD Written = 0;
		const bool bWritten = WriteFile(File, Bytes.GetData(), static_cast<DWORD>(Bytes.Num()), &Written, nullptr)
			&& Written == static_cast<DWORD>(Bytes.Num()) && FlushFileBuffers(File);
		CloseHandle(File);
		if (!bWritten)
		{
			DeleteFileW(Target.c_str());
			OutError = "Writing the new Physics Asset failed.";
		}
		return bWritten;
	}
}

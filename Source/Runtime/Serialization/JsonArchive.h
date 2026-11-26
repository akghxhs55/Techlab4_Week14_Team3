#pragma once

class UWorld;
class UObject;

class FJsonArchive
{
public:
	static bool SaveObject(UObject* Object, const FString& Path);
	static bool LoadObject(UObject* Object, const FString& Path, const FString& LegacyClassName = "");
	static bool SaveObjectToBytes(UObject* Object, TArray<uint8>& OutBytes);
	static bool LoadObjectFromBytes(UObject* Object, const TArray<uint8>& Bytes, const FString& LegacyClassName = "");
	static bool SaveWorld(UWorld* World, const FString& Path);
	static bool LoadWorld(UWorld* World, const FString& Path);
	static bool LoadWorldFromBytes(UWorld* World, const TArray<uint8>& Bytes);
	static bool SaveWorldToBytes(UWorld* World, TArray<uint8>& OutBytes);
};

#include "EnginePCH.h"
#include "SkeletalMesh.h"
#include "Render/RenderCommand.h"
#include "Asset/AssetManager.h"
#include "Asset/AssetUtils.h"
#include "Core/Misc/Paths.h"
#include "Physics/PhysicsAssetFile.h"
#include "json.hpp"
#include <fstream>

namespace
{
	TArray<FBox> BuildBoneBounds(const FSkeletalMeshLODRenderData& LOD, int32 NumBones)
	{
		TArray<FBox> Bounds;
		Bounds.SetNum(NumBones);
		for (FBox& Box : Bounds)
			Box = FBox{ FVector(FLT_MAX, FLT_MAX, FLT_MAX), FVector(-FLT_MAX, -FLT_MAX, -FLT_MAX) };

		for (int32 V = 0; V < LOD.BindVertices.Num(); ++V)
		{
			const FVector& Position = LOD.BindVertices[V].Position;
			const FSkinWeight& Weight = LOD.SkinWeights[V];
			for (int32 K = 0; K < MaxBoneInfluences; ++K)
			{
				if (Weight.Weight[K] <= 0.0f) continue;
				FBox& Box = Bounds[Weight.BoneIndex[K]];
				for (int Axis = 0; Axis < 3; ++Axis)
				{
					Box.Min[Axis] = std::min(Box.Min[Axis], Position[Axis]);
					Box.Max[Axis] = std::max(Box.Max[Axis], Position[Axis]);
				}
			}
		}
		return Bounds;
	}
}

bool USkeletalMesh::SetPhysicsAssetByPath(const FString& Path, FString& OutError)
{
	OutError.clear();
	if (Path.empty()) { SetPhysicsAsset(nullptr); return true; }
	const FString PreviewMesh = PhysicsAssetFile::ResolvePreviewMeshPath(Path, OutError);
	if (PreviewMesh.empty()) return false;
	if (_wcsicmp(FPaths::FromUtf8(PreviewMesh).c_str(), FPaths::FromUtf8(FAssetUtils::ResolveAssetFilePath(GetPath())).c_str()) != 0)
	{
		OutError = "This Physics Asset references a different skeletal mesh.";
		return false;
	}
	UPhysicsAsset* Asset = UAssetManager::LoadPhysicsAsset(Path, OutError);
	if (!Asset) return false;
	for (const UBodySetup* Body : Asset->BodySetups)
		if (!Body) { OutError = "The Physics Asset contains a null body."; return false; }
	if (!Asset->Validate(*this, Path))
	{
		OutError = "The Physics Asset contains invalid bodies or constraints. See Output Log.";
		return false;
	}
	SetPhysicsAsset(Asset);
	return true;
}

bool USkeletalMesh::LoadPhysicsAssetReference(FString& OutError)
{
	OutError.clear();
	const FString MeshFile = FAssetUtils::ResolveAssetFilePath(GetPath());
	if (MeshFile.empty()) { OutError = "Cannot resolve the skeletal mesh path."; return false; }
	const std::filesystem::path SettingsFile = FPaths::FromUtf8(MeshFile + ".physicsasset.json");
	std::error_code Error;
	const bool bHasSettings = std::filesystem::exists(SettingsFile, Error);
	if (Error) { OutError = "Cannot access the Physics Asset reference file."; return false; }
	if (bHasSettings)
	{
		std::ifstream File(SettingsFile, std::ios::binary);
		const json Settings = File ? json::parse(File, nullptr, false) : json();
		if (!Settings.is_object() || !Settings.contains("PhysicsAsset") || !Settings["PhysicsAsset"].is_string())
		{
			OutError = "Cannot read the saved Physics Asset reference: " + FPaths::ToUtf8(SettingsFile);
			return false;
		}
		return SetPhysicsAssetByPath(Settings["PhysicsAsset"].get<FString>(), OutError);
	}
	const FString LegacyPath = MeshFile + ".physics.json";
	if (std::filesystem::exists(FPaths::FromUtf8(LegacyPath), Error))
		return SetPhysicsAssetByPath(LegacyPath, OutError);
	if (Error) { OutError = "Cannot access the legacy Physics Asset."; return false; }
	SetPhysicsAsset(nullptr);
	return true;
}

bool USkeletalMesh::SavePhysicsAssetReference(FString& OutError) const
{
	OutError.clear();
	const FString MeshFile = FAssetUtils::ResolveAssetFilePath(GetPath());
	if (MeshFile.empty()) { OutError = "Cannot resolve the skeletal mesh path."; return false; }
	if (PhysicsAsset && PhysicsAsset->GetAssetPath().empty())
	{
		OutError = "Select a Physics Asset file before saving the default.";
		return false;
	}
	const json Settings = { { "Version", 1 }, { "PhysicsAsset", PhysicsAsset ? PhysicsAsset->GetAssetPath() : FString() } };
	const std::filesystem::path Target = FPaths::FromUtf8(MeshFile + ".physicsasset.json");
	std::filesystem::path Temporary = Target;
	Temporary += L".tmp." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64());
	std::ofstream File(Temporary, std::ios::binary | std::ios::trunc);
	if (!File) { OutError = "Cannot create the Physics Asset reference file."; return false; }
	File << Settings.dump(2);
	File.flush();
	File.close();
	const bool bWritten = static_cast<bool>(File);
	if (!bWritten || !MoveFileExW(Temporary.c_str(), Target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
	{
		std::error_code Error;
		std::filesystem::remove(Temporary, Error);
		OutError = "Cannot save the default Physics Asset. Check write access.";
		return false;
	}
	return true;
}

bool USkeletalMesh::Build(FSkeletalMeshData&& ImportData, FString& OutError)
{
	// CreateStaticMesh와 같은 기본값: 슬롯/섹션이 없으면 하나씩 채운다
	if (ImportData.MaterialSlots.IsEmpty())
	{
		FSkeletalMaterialSlot DefaultSlot;
		DefaultSlot.Name = "Default";
		ImportData.MaterialSlots.Add(DefaultSlot);
	}
	if (ImportData.Sections.IsEmpty())
	{
		FSkeletalMeshSection DefaultSection;
		DefaultSection.StartIndex = 0;
		DefaultSection.IndexCount = static_cast<uint32>(ImportData.Indices.Num());
		DefaultSection.MaterialSlotIndex = 0;
		ImportData.Sections.Add(DefaultSection);
	}

	if (!ImportData.Validate(OutError))
	{
		return false;
	}

	// 공유 IB. VB는 인스턴스마다 FSkeletalMeshObjectCPUSkin이 동적으로 만든다
	TUniquePtr<FIndexBuffer> IB = RenderCommand::CreateStaticIndexBuffer(ImportData.Indices.GetData(), static_cast<uint32>(ImportData.Indices.Num()));
	if (!IB)
	{
		OutError = "SkeletalMesh: index buffer creation failed";
		return false;
	}

	TArray<UMaterialInterface*> NewMaterials;
	if (!UAssetManager::BuildMaterialsFromSlots(ImportData.MaterialSlots, NewMaterials))
	{
		OutError = "SkeletalMesh: material creation failed";
		return false;
	}

	FSkeletalMeshLODRenderData LOD;
	LOD.BindVertices = std::move(ImportData.BindVertices);
	LOD.SkinWeights = std::move(ImportData.SkinWeight);
	LOD.Indices = std::move(ImportData.Indices);
	LOD.Sections = std::move(ImportData.Sections);
	LOD.IndexBuffer = std::move(IB);

	// 임포터가 채운 값에 의존하지 않고 바인드 정점에서 다시 계산
	LOD.BindAABB.Min = LOD.BindAABB.Max = LOD.BindVertices[0].Position;
	for (const FVertex& Vertex : LOD.BindVertices)
	{
		for (int Axis = 0; Axis < 3; ++Axis)
		{
			LOD.BindAABB.Min[Axis] = std::min(LOD.BindAABB.Min[Axis], Vertex.Position[Axis]);
			LOD.BindAABB.Max[Axis] = std::max(LOD.BindAABB.Max[Axis], Vertex.Position[Axis]);
		}
	}

	TArray<FGpuSkinVertex> GpuSkinVertices;
	GpuSkinVertices.SetNum(LOD.BindVertices.Num());
	for (int32 V = 0; V < GpuSkinVertices.Num(); ++V)
	{
		GpuSkinVertices[V].Vertex = LOD.BindVertices[V];
		for (int32 K = 0; K < MaxBoneInfluences; ++K)
		{
			GpuSkinVertices[V].BoneIndex[K] = LOD.SkinWeights[V].BoneIndex[K];
			GpuSkinVertices[V].Weight[K] = LOD.SkinWeights[V].Weight[K];
		}
	}
	LOD.GpuSkinVertexBuffer = RenderCommand::CreateStaticVertexBuffer(GpuSkinVertices.GetData(),
		sizeof(FGpuSkinVertex) * static_cast<uint32>(GpuSkinVertices.Num()), sizeof(FGpuSkinVertex));
	LOD.BoneBounds = BuildBoneBounds(LOD, ImportData.Bones.Num());

	RefSkeleton = std::move(ImportData.Bones);
	MaterialSlots = std::move(ImportData.MaterialSlots);
	Materials = std::move(NewMaterials);
	RenderData.LODRenderData.Empty();
	RenderData.LODRenderData.Add(std::move(LOD));
	RenderStates.Reset();
	return true;
}

int32 USkeletalMesh::FindBoneIndex(const FString& Name) const
{
	for (int32 I = 0; I < RefSkeleton.Num(); ++I)
	{
		if (RefSkeleton[I].Name == Name)
			return I;
	}
	return -1;
}

UMaterialInterface* USkeletalMesh::GetMaterial(uint32 SlotIndex) const
{
	if (SlotIndex < Materials.size() && Materials[SlotIndex])
	{
		return Materials[SlotIndex];
	}
	return UAssetManager::GetAssetByPath<UMaterial>("DefaultMaterial");
}

size_t USkeletalMesh::FMaterialSlotsHash::operator()(const TArray<UMaterialInterface*>& Slots) const
{
	size_t Hash = 0;
	for (const UMaterialInterface* Material : Slots)
		Hash ^= std::hash<const UMaterialInterface*>{}(Material) + 0x9e3779b9u + (Hash << 6) + (Hash >> 2);
	return Hash;
}

TSharedPtr<const FMeshRenderState> USkeletalMesh::GetRenderState(const TArray<UMaterialInterface*>& InMaterials)
{
	TWeakPtr<const FMeshRenderState>& WeakState = RenderStates[InMaterials];
	if (TSharedPtr<const FMeshRenderState> State = WeakState.lock()) return State;

	// LOD는 하나뿐이라 임계값 없이 LOD 0 범위만 채운다
	TSharedPtr<FMeshRenderState> State = MakeShared<FMeshRenderState>();
	if (!RenderData.LODRenderData.IsEmpty())
	{
		for (const FSkeletalMeshSection& Section : RenderData.LODRenderData[0].Sections)
		{
			UMaterialInterface* Material = Section.MaterialSlotIndex < InMaterials.size()
				? InMaterials[Section.MaterialSlotIndex] : GetMaterial(Section.MaterialSlotIndex);
			if (Material) State->Sections.Add({ Material, Section.StartIndex, Section.IndexCount });
		}
	}
	State->LODCount = 1;
	State->LODs[0] = { 0, static_cast<uint32>(State->Sections.Num()) };
	WeakState = State;
	return State;
}

int32 USkeletalMesh::FindSocketIndex(const FString& SocketName) const
{
	if (SocketName.empty())
		return -1;
	for (int32 Index = 0; Index < Sockets.Num(); ++Index)
	{
		if (Sockets[Index].SocketName == SocketName)
			return Index;
	}
	return -1;
}

const FSkeletalMeshSocket* USkeletalMesh::FindSocket(const FString& SocketName) const
{
	const int32 Index = FindSocketIndex(SocketName);
	return Index >= 0 ? &Sockets[Index] : nullptr;
}

FSkeletalMeshSocket* USkeletalMesh::FindMutableSocket(const FString& SocketName)
{
	const int32 Index = FindSocketIndex(SocketName);
	return Index >= 0 ? &Sockets[Index] : nullptr;
}

bool USkeletalMesh::HasSocket(const FString& SocketName) const
{
	return FindSocketIndex(SocketName) >= 0;
}

bool USkeletalMesh::AddSocket(const FSkeletalMeshSocket& NewSocket)
{
	if (NewSocket.SocketName.empty() || NewSocket.BoneName.empty() || HasSocket(NewSocket.SocketName))
		return false;
	if (FindBoneIndex(NewSocket.BoneName) < 0)
		return false;
	Sockets.Add(NewSocket);
	return true;
}

bool USkeletalMesh::RemoveSocket(const FString& SocketName)
{
	const int32 Index = FindSocketIndex(SocketName);
	if (Index < 0)
		return false;
	Sockets.RemoveAt(Index, 1);
	return true;
}

bool USkeletalMesh::LoadSocketsFromFile(const FString& FilePath)
{
	if (FilePath.empty())
		return false;
	std::ifstream File(FilePath);
	if (!File.is_open())
		return false;
	json Root = json::parse(File, nullptr, false);
	if (!Root.is_object() || !Root.contains("Sockets") || !Root["Sockets"].is_array())
		return false;

	Sockets.Reset();
	for (const json& Item : Root["Sockets"])
	{
		if (!Item.is_object())
			continue;
		FSkeletalMeshSocket Socket;
		Socket.SocketName = Item.value("SocketName", FString{});
		Socket.BoneName = Item.value("BoneName", FString{});
		if (Item.contains("RelativeLocation") && Item["RelativeLocation"].is_array() && Item["RelativeLocation"].size() == 3)
		{
			Socket.RelativeLocation = FVector(
				Item["RelativeLocation"][0].get<float>(),
				Item["RelativeLocation"][1].get<float>(),
				Item["RelativeLocation"][2].get<float>());
		}
		if (Item.contains("RelativeRotation") && Item["RelativeRotation"].is_array() && Item["RelativeRotation"].size() == 3)
		{
			Socket.RelativeRotation = FRotator(
				Item["RelativeRotation"][0].get<float>(),
				Item["RelativeRotation"][1].get<float>(),
				Item["RelativeRotation"][2].get<float>());
		}
		if (Item.contains("RelativeScale") && Item["RelativeScale"].is_array() && Item["RelativeScale"].size() == 3)
		{
			Socket.RelativeScale = FVector(
				Item["RelativeScale"][0].get<float>(),
				Item["RelativeScale"][1].get<float>(),
				Item["RelativeScale"][2].get<float>());
		}
		AddSocket(Socket);
	}
	return true;
}

bool USkeletalMesh::SaveSocketsToFile(const FString& FilePath) const
{
	if (FilePath.empty())
		return false;
	json Root = json::object();
	json Array = json::array();
	for (const FSkeletalMeshSocket& Socket : Sockets)
	{
		json Entry = json::object();
		Entry["SocketName"] = Socket.SocketName;
		Entry["BoneName"] = Socket.BoneName;
		Entry["RelativeLocation"] = { Socket.RelativeLocation.X, Socket.RelativeLocation.Y, Socket.RelativeLocation.Z };
		Entry["RelativeRotation"] = { Socket.RelativeRotation.Pitch, Socket.RelativeRotation.Yaw, Socket.RelativeRotation.Roll };
		Entry["RelativeScale"] = { Socket.RelativeScale.X, Socket.RelativeScale.Y, Socket.RelativeScale.Z };
		Array.push_back(std::move(Entry));
	}
	Root["Sockets"] = std::move(Array);
	std::ofstream File(FilePath, std::ios::trunc);
	if (!File.is_open())
		return false;
	File << Root.dump(2);
	return !File.fail();
}

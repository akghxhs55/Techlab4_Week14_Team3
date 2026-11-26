#pragma once

#include "Render/SkeletalMeshData.h"
#include "Render/Vertex.h"

class UPhysicsAsset;
class UBodySetup;
class USkeletalMesh;

struct FPhysicsAssetGenInput
{
	const TArray<FBoneInfo>* Bones = nullptr;
	const TArray<FVertex>* BindVertices = nullptr;
	const TArray<FSkinWeight>* SkinWeights = nullptr;

	bool IsValid(FString& OutError) const;

	static FPhysicsAssetGenInput FromImportData(const FSkeletalMeshData& Data);
	static FPhysicsAssetGenInput FromMesh(const USkeletalMesh& Mesh);
};

enum class EPhysicsAssetGenGeomType : uint8
{
	Capsule,
};

struct FPhysicsAssetGenSettings
{
	float MinBoneLength = 5.0f;
	int32 MinVertexCount = 10;
	float MinWeight = 0.5f;
	float RadiusPercentile = 0.9f;
	float RadiusScale = 0.9f;
	EPhysicsAssetGenGeomType GeomType = EPhysicsAssetGenGeomType::Capsule;
	bool bCreateConstraints = true;
};

namespace FPhysicsAssetGenerator
{
	bool Generate(UPhysicsAsset& Out, const FPhysicsAssetGenInput& In,
		const FPhysicsAssetGenSettings& Settings, TArray<FString>& OutWarnings);

	UBodySetup* GenerateBodyForBone(UPhysicsAsset& Asset, const FPhysicsAssetGenInput& In,
		int32 BoneIndex, const FPhysicsAssetGenSettings& Settings);

	void RebuildConstraints(UPhysicsAsset& Asset, const TArray<FBoneInfo>& Bones);
}

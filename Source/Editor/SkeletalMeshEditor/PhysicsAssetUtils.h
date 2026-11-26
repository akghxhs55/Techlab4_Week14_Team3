#pragma once

#include "Core/EngineString.h"

class USkeletalMesh;
class UPhysicsAsset;

enum class EPhysAssetFitGeomType : uint8
{
	Capsule,
	Box
};

enum class EPhysAssetAngularMode : uint8
{
	Limited,
	Locked,
	Free
};

struct FPhysAssetCreateParams
{
	float MinBoneSize = 0.08f;
	int32 MinVertexCount = 8;
	EPhysAssetFitGeomType GeomType = EPhysAssetFitGeomType::Capsule;
	EPhysAssetAngularMode AngularMode = EPhysAssetAngularMode::Limited;
	bool bAutoOrientToBone = true;
	bool bIncludeChildBones = true;
};

struct FPhysicsAssetUtils
{
	static bool CreateFromSkeletalMesh(const USkeletalMesh& Mesh, UPhysicsAsset& Asset,
		const FPhysAssetCreateParams& Params, FString& OutError);
};

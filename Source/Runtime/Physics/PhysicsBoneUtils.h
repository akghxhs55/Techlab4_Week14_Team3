#pragma once

#include "Render/SkeletalMeshData.h"

namespace FPhysicsBoneUtils
{
	FVector FindBoneAxis(const TArray<FBoneInfo>& Bones, int32 Bone);
}

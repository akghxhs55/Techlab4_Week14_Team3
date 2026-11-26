#include "EnginePCH.h"
#include "PhysicsBoneUtils.h"

#include <cmath>

FVector FPhysicsBoneUtils::FindBoneAxis(const TArray<FBoneInfo>& Bones, int32 Bone)
{
	for (int32 B = Bone; B >= 0; B = Bones[B].ParentIndex)
	{
		int32 BestChild = -1;
		float BestLengthSq = 0.0f;
		for (int32 C = B + 1; C < Bones.Num(); ++C)
		{
			if (Bones[C].ParentIndex != B) continue;

			const FVector& L = Bones[C].LocalBindPose.Location;
			const float AX = std::abs(L.X), AY = std::abs(L.Y), AZ = std::abs(L.Z);
			if (AX + AY + AZ < 1e-3f) continue;

			const float LengthSq = L.X * L.X + L.Y * L.Y + L.Z * L.Z;
			if (LengthSq > BestLengthSq)
			{
				BestLengthSq = LengthSq;
				BestChild = C;
			}
		}

		if (BestChild >= 0)
		{
			const FVector& L = Bones[BestChild].LocalBindPose.Location;
			const float AX = std::abs(L.X), AY = std::abs(L.Y), AZ = std::abs(L.Z);
			if (AX >= AY && AX >= AZ) return { L.X > 0 ? 1.0f : -1.0f, 0.0f, 0.0f };
			if (AY >= AZ)             return { 0.0f, L.Y > 0 ? 1.0f : -1.0f, 0.0f };
			return { 0.0f, 0.0f, L.Z > 0 ? 1.0f : -1.0f };
		}
	}
	return { 1.0f, 0.0f, 0.0f };
}

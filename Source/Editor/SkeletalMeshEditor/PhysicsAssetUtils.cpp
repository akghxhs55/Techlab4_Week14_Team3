#include "EnginePCH.h"
#include "PhysicsAssetUtils.h"

#include "Physics/BodySetup.h"
#include "Physics/PhysicsAsset.h"
#include "Render/SkeletalMesh.h"
#include "UObject/UObjectGlobals.h"
#include <algorithm>
#include <cctype>
#include <cfloat>

bool FPhysicsAssetUtils::CreateFromSkeletalMesh(const USkeletalMesh& Mesh, UPhysicsAsset& Asset,
	const FPhysAssetCreateParams& Params, FString& OutError)
{
	OutError.clear();
	if (Params.MinBoneSize <= 0.0f || Params.MinVertexCount < 1)
	{
		OutError = "Invalid Physics Asset generation settings";
		return false;
	}
	const TArray<FBoneInfo>& Bones = Mesh.GetRefSkeleton();
	const auto& LODs = Mesh.GetRenderData().LODRenderData;
	if (Bones.IsEmpty() || LODs.IsEmpty() || !Asset.BodySetups.IsEmpty() || !Asset.Constraints.IsEmpty())
	{
		OutError = "Physics Asset generation requires a mesh and an empty asset";
		return false;
	}
	const FSkeletalMeshLODRenderData& LOD = LODs[0];
	if (LOD.BindVertices.Num() != LOD.SkinWeights.Num())
	{
		OutError = "Skeletal mesh vertex and skin weight counts differ";
		return false;
	}
	TArray<FBox> LocalBounds;
	TArray<int32> VertexCounts;
	TArray<int32> VertexBones;
	LocalBounds.SetNum(Bones.Num());
	VertexCounts.SetNum(Bones.Num());
	VertexBones.SetNum(LOD.BindVertices.Num());
	for (int32 Index = 0; Index < Bones.Num(); ++Index)
	{
		LocalBounds[Index] = { FVector(FLT_MAX, FLT_MAX, FLT_MAX), FVector(-FLT_MAX, -FLT_MAX, -FLT_MAX) };
		VertexCounts[Index] = 0;
	}
	for (int32& BoneIndex : VertexBones) BoneIndex = -1;
	for (int32 VertexIndex = 0; VertexIndex < LOD.BindVertices.Num(); ++VertexIndex)
	{
		const FSkinWeight& Skin = LOD.SkinWeights[VertexIndex];
		int32 BoneIndex = -1;
		float GreatestWeight = 0.0f;
		for (int32 Influence = 0; Influence < MaxBoneInfluences; ++Influence)
		{
			if (Skin.Weight[Influence] > GreatestWeight && Skin.BoneIndex[Influence] < Bones.Num())
			{
				GreatestWeight = Skin.Weight[Influence];
				BoneIndex = Skin.BoneIndex[Influence];
			}
		}
		if (BoneIndex < 0) continue;
		VertexBones[VertexIndex] = BoneIndex;
		const FVector Local = Bones[BoneIndex].InverseBindPose.TransformPosition(LOD.BindVertices[VertexIndex].Position);
		FBox& Bounds = LocalBounds[BoneIndex];
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			Bounds.Min[Axis] = std::min(Bounds.Min[Axis], Local[Axis]);
			Bounds.Max[Axis] = std::max(Bounds.Max[Axis], Local[Axis]);
		}
		++VertexCounts[BoneIndex];
	}
	TArray<int32> SubtreeVertexCounts = VertexCounts;
	for (int32 Index = Bones.Num() - 1; Index >= 0; --Index)
		if (const int32 Parent = Bones[Index].ParentIndex; Parent >= 0)
			SubtreeVertexCounts[Parent] += SubtreeVertexCounts[Index];
	TArray<uint8> HasBody;
	TArray<FVector> BoneDirections;
	TArray<FQuat> BodyOrientations;
	HasBody.SetNum(Bones.Num());
	BoneDirections.SetNum(Bones.Num());
	BodyOrientations.SetNum(Bones.Num());
	for (int32 Index = 0; Index < Bones.Num(); ++Index)
	{
		HasBody[Index] = 0;
		BoneDirections[Index] = FVector::ZeroVector;
		BodyOrientations[Index] = FQuat::Identity();
		const int32 CandidateVertices = Params.bIncludeChildBones ? SubtreeVertexCounts[Index] : VertexCounts[Index];
		if (CandidateVertices < Params.MinVertexCount) continue;
		FString LowerName = Bones[Index].Name;
		std::transform(LowerName.begin(), LowerName.end(), LowerName.begin(),
			[](unsigned char Ch) { return static_cast<char>(std::tolower(Ch)); });
		if (Bones[Index].ParentIndex < 0 && LowerName == "root" && VertexCounts[Index] == 0) continue;
		if (LowerName.find("twist") != FString::npos || LowerName.starts_with("ik_")) continue;
		const FVector Size = VertexCounts[Index] > 0
			? LocalBounds[Index].Max - LocalBounds[Index].Min : FVector::ZeroVector;
		float LongestChild = 0.0f;
		float SecondChild = 0.0f;
		for (int32 Child = Index + 1; Child < Bones.Num(); ++Child)
			if (Bones[Child].ParentIndex == Index)
			{
				const float Length = Bones[Child].LocalBindPose.Location.Size();
				if (Length > LongestChild)
				{
					SecondChild = LongestChild;
					LongestChild = Length;
					BoneDirections[Index] = Bones[Child].LocalBindPose.Location;
				}
				else SecondChild = std::max(SecondChild, Length);
			}
		if (std::max({ Size.X, Size.Y, Size.Z, LongestChild }) < Params.MinBoneSize) continue;
		if (Params.bAutoOrientToBone || Params.GeomType == EPhysAssetFitGeomType::Capsule)
		{
			FVector Axis = BoneDirections[Index];
			if (!Params.bAutoOrientToBone || LongestChild <= SecondChild * 1.2f || Axis.SizeSquared() <= SMALL_NUMBER)
			{
				const int32 LongAxis = Size.Y > Size.X ? (Size.Z > Size.Y ? 2 : 1) : (Size.Z > Size.X ? 2 : 0);
				Axis = LongAxis == 0 ? FVector(1, 0, 0) : LongAxis == 1 ? FVector(0, 1, 0) : FVector(0, 0, 1);
				BoneDirections[Index] = FVector::ZeroVector;
			}
			Axis = Axis.GetSafeNormal();
			BodyOrientations[Index] = Axis.Z < -0.999f
				? FQuat::MakeFromAxisAngle(FVector(1, 0, 0), PI)
				: FQuat(-Axis.Y, Axis.X, 0.0f, 1.0f + Axis.Z).Normalized();
		}
		HasBody[Index] = 1;
	}
	if (Params.bIncludeChildBones)
	{
		bool bRemovedEmptyBody;
		do
		{
			bRemovedEmptyBody = false;
			TArray<int32> AssignedCounts;
			AssignedCounts.SetNum(Bones.Num());
			for (int32& Count : AssignedCounts) Count = 0;
			for (int32 SourceBone : VertexBones)
			{
				int32 OwnerBone = SourceBone;
				while (OwnerBone >= 0 && !HasBody[OwnerBone]) OwnerBone = Bones[OwnerBone].ParentIndex;
				if (OwnerBone >= 0) ++AssignedCounts[OwnerBone];
			}
			for (int32 Index = 0; Index < Bones.Num(); ++Index)
				if (HasBody[Index] && AssignedCounts[Index] == 0)
				{
					HasBody[Index] = 0;
					bRemovedEmptyBody = true;
				}
		} while (bRemovedEmptyBody);
	}
	TArray<FBox> FitBounds;
	FitBounds.SetNum(Bones.Num());
	for (int32 Index = 0; Index < Bones.Num(); ++Index)
		FitBounds[Index] = { FVector(FLT_MAX, FLT_MAX, FLT_MAX), FVector(-FLT_MAX, -FLT_MAX, -FLT_MAX) };
	for (int32 VertexIndex = 0; VertexIndex < LOD.BindVertices.Num(); ++VertexIndex)
	{
		int32 BoneIndex = VertexBones[VertexIndex];
		if (BoneIndex < 0) continue;
		const int32 SourceBone = BoneIndex;
		while (BoneIndex >= 0 && !HasBody[BoneIndex]) BoneIndex = Bones[BoneIndex].ParentIndex;
		if (BoneIndex < 0 || (!Params.bIncludeChildBones && BoneIndex != SourceBone)) continue;
		const FVector Local = Bones[BoneIndex].InverseBindPose.TransformPosition(LOD.BindVertices[VertexIndex].Position);
		const FVector Aligned = BodyOrientations[BoneIndex].Inverse().RotateVector(Local);
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			FitBounds[BoneIndex].Min[Axis] = std::min(FitBounds[BoneIndex].Min[Axis], Aligned[Axis]);
			FitBounds[BoneIndex].Max[Axis] = std::max(FitBounds[BoneIndex].Max[Axis], Aligned[Axis]);
		}
	}
	for (int32 Index = 0; Index < Bones.Num(); ++Index)
	{
		if (!HasBody[Index]) continue;
		FBox& Bounds = FitBounds[Index];
		const FVector End = BodyOrientations[Index].Inverse().RotateVector(BoneDirections[Index]);
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			Bounds.Min[Axis] = std::min({ Bounds.Min[Axis], 0.0f, End[Axis] });
			Bounds.Max[Axis] = std::max({ Bounds.Max[Axis], 0.0f, End[Axis] });
		}
		const FVector Size = Bounds.Max - Bounds.Min;
		const FVector Center = BodyOrientations[Index].RotateVector((Bounds.Min + Bounds.Max) * 0.5f);
		UBodySetup* Setup = NewObject<UBodySetup>(&Asset);
		Setup->BoneName = Bones[Index].Name;
		if (Params.GeomType == EPhysAssetFitGeomType::Capsule)
		{
			FCapsuleElem Capsule;
			Capsule.Center = Center;
			Capsule.Rotation = BodyOrientations[Index].ToFRotator();
			Capsule.Radius = std::max(std::max(Size.X, Size.Y) * 0.5f, 0.02f);
			Capsule.Length = std::max(Size.Z - 2.0f * Capsule.Radius, 0.0f);
			Setup->AggGeom.CapsuleElems.Add(Capsule);
		}
		else
		{
			FBoxElem Box;
			Box.Center = Center;
			Box.Rotation = BodyOrientations[Index].ToFRotator();
			Box.X = std::max(Size.X, 0.04f);
			Box.Y = std::max(Size.Y, 0.04f);
			Box.Z = std::max(Size.Z, 0.04f);
			Setup->AggGeom.BoxElems.Add(Box);
		}
		Asset.BodySetups.Add(Setup);
	}
	if (Asset.BodySetups.IsEmpty())
	{
		OutError = "No bones have enough skinned vertices to create physics bodies";
		return false;
	}
	for (int32 Index = 0; Index < Bones.Num(); ++Index)
	{
		if (!HasBody[Index]) continue;
		int32 Parent = Bones[Index].ParentIndex;
		while (Parent >= 0 && !HasBody[Parent]) Parent = Bones[Parent].ParentIndex;
		if (Parent < 0) continue;
		FPhysicsAssetConstraint Constraint;
		Constraint.ChildBone = Bones[Index].Name;
		Constraint.ParentBone = Bones[Parent].Name;
		const EConstraintMotion Motion = Params.AngularMode == EPhysAssetAngularMode::Locked
			? EConstraintMotion::Locked : Params.AngularMode == EPhysAssetAngularMode::Free
			? EConstraintMotion::Free : EConstraintMotion::Limited;
		Constraint.TwistMotion = Motion;
		Constraint.Swing1Motion = Motion;
		Constraint.Swing2Motion = Motion;
		if (Params.AngularMode == EPhysAssetAngularMode::Limited)
		{
			FString ChildName = Bones[Index].Name;
			FString ParentName = Bones[Parent].Name;
			std::transform(ChildName.begin(), ChildName.end(), ChildName.begin(),
				[](unsigned char Ch) { return static_cast<char>(std::tolower(Ch)); });
			std::transform(ParentName.begin(), ParentName.end(), ParentName.begin(),
				[](unsigned char Ch) { return static_cast<char>(std::tolower(Ch)); });
			if ((ChildName.find("calf") != FString::npos || ChildName.find("shin") != FString::npos ||
				ChildName.find("lowerleg") != FString::npos) &&
				(ParentName.find("thigh") != FString::npos || ParentName.find("upperleg") != FString::npos))
			{
				Constraint.FrameOffset = FRotator(-90.0f, 0.0f, 0.0f);
				Constraint.TwistMinDeg = -5.0f;
				Constraint.TwistMaxDeg = 110.0f;
				Constraint.Swing1Motion = EConstraintMotion::Locked;
				Constraint.Swing2Motion = EConstraintMotion::Locked;
			}
		}
		Asset.Constraints.Add(Constraint);
	}
	return Asset.Validate(Mesh, OutError);
}

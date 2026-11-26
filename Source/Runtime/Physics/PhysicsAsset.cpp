#include "EnginePCH.h"
#include "PhysicsAsset.h"

#include "BodySetup.h"
#include "Render/SkeletalMesh.h"
#include "Render/SkeletalMeshData.h"

UPhysicsAsset::~UPhysicsAsset()
{
	ClearBodies();
}

void UPhysicsAsset::ClearBodies()
{
	TArray<UBodySetup*> ToDelete = BodySetups;
	BodySetups.Empty();
	for (UBodySetup* Body : ToDelete)
	{
		delete Body;
	}
}

void UPhysicsAsset::Serialize(FStructuredArchive::FRecord Record)
{
	UObject::Serialize(Record);

	FArchive& Ar = Record.GetUnderlyingArchive();
	if (Ar.IsLoading()) PreviewMeshPath.clear();
	if (auto Field = Record.TryEnterField("PreviewMeshPath", true))
	{
		Field.GetValue() << PreviewMeshPath;
	}
	int32 Num = BodySetups.Num();
	FStructuredArchive::FArray Array = Record.EnterArray("Bodies", Num);

	if (Ar.IsLoading())
	{
		ClearBodies();
		BodySetups.Reserve(Num);
	}
	for (int32 i = 0; i < Num; ++i)
	{
		UBodySetup* Body = Ar.IsLoading() ? NewObject<UBodySetup>(this) : BodySetups[i];
		Body->Serialize(Array.EnterElement().EnterRecord());
		if (Ar.IsLoading())
		{
			BodySetups.Add(Body);
		}
	}
}

bool UPhysicsAsset::Validate(const USkeletalMesh& Mesh, const FString& DebugName) const
{
	return Validate(Mesh.GetRefSkeleton(), DebugName);
}

bool UPhysicsAsset::Validate(const TArray<FBoneInfo>& Bones, const FString& DebugName) const
{
	const auto FindBoneIndex = [&Bones](const FString& Name)
	{
		for (int32 I = 0; I < Bones.Num(); ++I)
		{
			if (Bones[I].Name == Name) return I;
		}
		return -1;
	};

	TArray<int32> BodyOfBone;
	BodyOfBone.Init(-1, Bones.Num());
	int32 NumProblems = 0;

    for (int32 i = 0; i < BodySetups.Num(); ++i)
    {
        const UBodySetup* Body = BodySetups[i];
        const int32 Bone = FindBoneIndex(Body->BoneName);
        if (Bone < 0)
        {
            KRS_LOG(Warning, "[PhysicsAsset] {}: body bone '{}' not found", DebugName, Body->BoneName);
            ++NumProblems; continue;
        }
        if (BodyOfBone[Bone] >= 0)
        {
            KRS_LOG(Warning, "[PhysicsAsset] {}: duplicate body on bone '{}'", DebugName, Body->BoneName);
            ++NumProblems; continue;
        }
        if (Body->AggGeom.GetElementCount() == 0)
        {
            KRS_LOG(Warning, "[PhysicsAsset] {}: body '{}' has no shapes", DebugName, Body->BoneName);
            ++NumProblems;
        }
        for (const FCapsuleElem& C : Body->AggGeom.CapsuleElems)
        {
	        if (C.Radius <= 0.0f || C.Length < 0.0f)
	        {
	        	KRS_LOG(Warning, "[PhysicsAsset] {}: body '{}' has invalid capsule", DebugName, Body->BoneName);
	        	++NumProblems;
	        }
        }
        BodyOfBone[Bone] = i;
    }

    TArray<uint8> BodyHasConstraint;
    BodyHasConstraint.Init(0, BodySetups.Num());
    for (const FPhysicsAssetConstraint& C : Constraints)
    {
        const int32 Child = FindBoneIndex(C.ChildBone);
        const int32 Parent = FindBoneIndex(C.ParentBone);
        if (Child < 0 || Parent < 0 || BodyOfBone[Child] < 0 || BodyOfBone[Parent] < 0)
        {
            KRS_LOG(Warning, "[PhysicsAsset] {}: constraint '{}'-'{}' refers to missing bone/body", DebugName, C.ChildBone, C.ParentBone);
            ++NumProblems; continue;
        }
        if (BodyHasConstraint[BodyOfBone[Child]])
        {
            KRS_LOG(Warning, "[PhysicsAsset] {}: bone '{}' has more than one constraint", DebugName, C.ChildBone);
            ++NumProblems;
        }
        BodyHasConstraint[BodyOfBone[Child]] = 1;

        int32 Nearest = Bones[Child].ParentIndex;
        while (Nearest >= 0 && BodyOfBone[Nearest] < 0)
            Nearest = Bones[Nearest].ParentIndex;

        if (Nearest != Parent)
        {
            bool bIsAncestor = false;
            for (int32 A = Bones[Child].ParentIndex; A >= 0; A = Bones[A].ParentIndex)
            {
	            if (A == Parent)
	            {
		            bIsAncestor = true; break;
	            }
            }

            KRS_LOG(Warning, "[PhysicsAsset] {}: constraint '{}' parent '{}' is {} (expected '{}')",
                DebugName, C.ChildBone, C.ParentBone,
                bIsAncestor ? "not the nearest body ancestor" : "not an ancestor",
                Nearest >= 0 ? Bones[Nearest].Name : FString("none"));
            ++NumProblems;
        }
    }

    int32 NumRootBodies = 0;
    for (int32 B = 0; B < BodySetups.Num(); ++B)
    {
        const int32 Bone = FindBoneIndex(BodySetups[B]->BoneName);
        if (Bone < 0 || BodyOfBone[Bone] != B || BodyHasConstraint[B])
        {
	        continue;
        }
        if (++NumRootBodies > 1)
        {
            KRS_LOG(Warning, "[PhysicsAsset] {}: body '{}' has no constraint (extra root)", DebugName, BodySetups[B]->BoneName);
            ++NumProblems;
        }
    }

    KRS_LOG(Info, "[PhysicsAsset] {}: bodies {}, constraints {}, problems {}", DebugName, BodySetups.Num(), Constraints.Num(), NumProblems);
    return NumProblems == 0;
}

#include "EnginePCH.h"
#include "Editor/Physics/PhysicsAssetGenerator.h"

#include "Physics/BodySetup.h"
#include "Physics/PhysicsAsset.h"
#include "Physics/PhysicsBoneUtils.h"
#include "Render/SkeletalMesh.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstring>

namespace
{
	FString ToLowerAscii(const FString& Value)
	{
		FString Lower = Value;
		for (char& Ch : Lower)
		{
			if (Ch >= 'A' && Ch <= 'Z') Ch = static_cast<char>(Ch + ('a' - 'A'));
		}
		return Lower;
	}

	bool ContainsWordToken(const FString& LowerName, const char* Token)
	{
		const size_t Len = std::strlen(Token);
		size_t Pos = 0;
		while ((Pos = LowerName.find(Token, Pos)) != FString::npos)
		{
			const bool bLeftOk = (Pos == 0) || !std::isalpha(static_cast<unsigned char>(LowerName[Pos - 1]));
			const size_t End = Pos + Len;
			const bool bRightOk = (End >= LowerName.size()) || !std::isalpha(static_cast<unsigned char>(LowerName[End]));
			if (bLeftOk && bRightOk) return true;
			Pos = End;
		}
		return false;
	}

	// 트위스트나 보조 본 및 말단 소관절을 제외해 주요 골격 단위로 바디를 생성한다
	bool IsExcludedBoneName(const FString& Name)
	{
		const FString Lower = ToLowerAscii(Name);
		static constexpr const char* ExcludedSubstrings[] = {
			"twist", "share", "ik_", "_ik", "facial", "jaw", "tongue", "teeth",
			"eye", "brow", "lid", "lip", "cheek", "nose", "hair", "breast",
			"toe", "thumb", "index", "mid", "ring", "pinky", "clavicle"
		};
		for (const char* Sub : ExcludedSubstrings)
		{
			if (Lower.find(Sub) != FString::npos) return true;
		}
		return ContainsWordToken(Lower, "ear");
	}

	float GetBoneLength(const TArray<FBoneInfo>& Bones, int32 Bone)
	{
		float Length = 0.0f;
		for (int32 C = Bone + 1; C < Bones.Num(); ++C)
		{
			if (Bones[C].ParentIndex != Bone) continue;
			const FVector& L = Bones[C].LocalBindPose.Location;
			if (std::abs(L.X) + std::abs(L.Y) + std::abs(L.Z) < 1e-3f) continue;
			Length = FMath::Max(Length, L.Length());
		}
		return Length;
	}

	int32 FindDominantBone(const FSkinWeight& Weight, float MinWeight)
	{
		float MaxWeight = 0.0f;
		int32 Dominant = -1;
		for (int32 I = 0; I < MaxBoneInfluences; ++I)
		{
			const float W = Weight.Weight[I];
			if (W > 0.0f && W >= MinWeight && W > MaxWeight)
			{
				MaxWeight = W;
				Dominant = Weight.BoneIndex[I];
			}
		}
		return Dominant;
	}

	TArray<float> ComputeBoneVertexExtents(const FPhysicsAssetGenInput& In, float MinWeight)
	{
		const TArray<FBoneInfo>& Bones = *In.Bones;
		const int32 NumBones = Bones.Num();
		TArray<FBox> Boxes;
		Boxes.SetNum(NumBones);
		TArray<uint8> HasPts;
		HasPts.Init(0, NumBones);
		for (int32 B = 0; B < NumBones; ++B)
		{
			Boxes[B] = FBox{ FVector(FLT_MAX, FLT_MAX, FLT_MAX), FVector(-FLT_MAX, -FLT_MAX, -FLT_MAX) };
		}

		for (int32 V = 0; V < In.BindVertices->Num(); ++V)
		{
			int32 Target = FindDominantBone((*In.SkinWeights)[V], MinWeight);
			while (Target >= 0 && IsExcludedBoneName(Bones[Target].Name))
			{
				Target = Bones[Target].ParentIndex;
			}
			if (Target < 0) continue;

			const FVector Local = Bones[Target].InverseBindPose.TransformPosition((*In.BindVertices)[V].Position);
			FBox& Box = Boxes[Target];
			Box.Min.X = FMath::Min(Box.Min.X, Local.X);
			Box.Min.Y = FMath::Min(Box.Min.Y, Local.Y);
			Box.Min.Z = FMath::Min(Box.Min.Z, Local.Z);
			Box.Max.X = FMath::Max(Box.Max.X, Local.X);
			Box.Max.Y = FMath::Max(Box.Max.Y, Local.Y);
			Box.Max.Z = FMath::Max(Box.Max.Z, Local.Z);
			HasPts[Target] = 1;
		}

		TArray<float> Extents;
		Extents.Init(0.0f, NumBones);
		for (int32 B = 0; B < NumBones; ++B)
		{
			if (!HasPts[B]) continue;
			const FVector Size = Boxes[B].Max - Boxes[B].Min;
			Extents[B] = FMath::Max(Size.X, FMath::Max(Size.Y, Size.Z));
		}
		return Extents;
	}

	int32 FindNearestBodyAncestor(const TArray<FBoneInfo>& Bones, const TArray<uint8>& HasBody, int32 Bone)
	{
		for (int32 P = Bones[Bone].ParentIndex; P >= 0; P = Bones[P].ParentIndex)
		{
			if (HasBody[P]) return P;
		}
		return -1;
	}

	TArray<uint8> GetBodyBonesFromAsset(const UPhysicsAsset& Asset, const TArray<FBoneInfo>& Bones)
	{
		TArray<uint8> HasBody;
		HasBody.Init(0, Bones.Num());
		for (const UBodySetup* Body : Asset.BodySetups)
		{
			if (!Body) continue;
			for (int32 B = 0; B < Bones.Num(); ++B)
			{
				if (Bones[B].Name == Body->BoneName) { HasBody[B] = 1; break; }
			}
		}
		return HasBody;
	}

	TArray<TArray<FVector>> GatherBodyPoints(const FPhysicsAssetGenInput& In, const TArray<uint8>& HasBody, float MinWeight)
	{
		const TArray<FBoneInfo>& Bones = *In.Bones;
		TArray<TArray<FVector>> Points;
		Points.SetNum(Bones.Num());
		for (int32 V = 0; V < In.BindVertices->Num(); ++V)
		{
			const int32 Dominant = FindDominantBone((*In.SkinWeights)[V], MinWeight);
			if (Dominant < 0) continue;

			const int32 Target = HasBody[Dominant] ? Dominant : FindNearestBodyAncestor(Bones, HasBody, Dominant);
			if (Target < 0) continue;

			Points[Target].Add(Bones[Target].InverseBindPose.TransformPosition((*In.BindVertices)[V].Position));
		}
		return Points;
	}

	float Percentile(TArray<float> Values, float Fraction)
	{
		const int32 Num = Values.Num();
		const int32 K = FMath::Clamp(static_cast<int32>(Fraction * static_cast<float>(Num - 1)), 0, Num - 1);
		float* Data = Values.GetData();
		std::nth_element(Data, Data + K, Data + Num);
		return Data[K];
	}

	FCapsuleElem FitCapsuleAlongAxis(const TArray<FVector>& LocalPoints, const FVector& Axis,
		const FPhysicsAssetGenSettings& Settings, float MinRadius, float& OutRawRadius, float& OutExtent)
	{
		constexpr float AxialTrim = 0.02f;
		constexpr float PerpTrim = 0.05f;
		const int32 Num = LocalPoints.Num();

		FCapsuleElem Capsule{};
		const FVector Z(0.0f, 0.0f, 1.0f);
		const float Dot = FMath::Clamp(Z.Dot(Axis), -1.0f, 1.0f);
		if (Dot > 0.9999f) Capsule.Rotation = FRotator();
		else if (Dot < -0.9999f) Capsule.Rotation = FQuat::MakeFromAxisAngle(FVector(1.0f, 0.0f, 0.0f), PI).ToFRotator();
		else Capsule.Rotation = FQuat::MakeFromAxisAngle(Z.Cross(Axis), std::acos(Dot)).ToFRotator();

		TArray<float> AxialPositions;
		TArray<float> PerpX;
		TArray<float> PerpY;
		TArray<float> PerpZ;
		AxialPositions.SetNum(Num);
		PerpX.SetNum(Num);
		PerpY.SetNum(Num);
		PerpZ.SetNum(Num);
		for (int32 I = 0; I < Num; ++I)
		{
			AxialPositions[I] = LocalPoints[I].Dot(Axis);
			const FVector Perp = LocalPoints[I] - Axis * AxialPositions[I];
			PerpX[I] = Perp.X;
			PerpY[I] = Perp.Y;
			PerpZ[I] = Perp.Z;
		}
		// 머리카락 등 편향 정점 영향을 줄이기 위해 상하위 백분위 중간점을 중심으로 사용한다
		const FVector PerpCenter(
			0.5f * (Percentile(PerpX, PerpTrim) + Percentile(PerpX, 1.0f - PerpTrim)),
			0.5f * (Percentile(PerpY, PerpTrim) + Percentile(PerpY, 1.0f - PerpTrim)),
			0.5f * (Percentile(PerpZ, PerpTrim) + Percentile(PerpZ, 1.0f - PerpTrim)));

		TArray<float> RadialDistances;
		RadialDistances.SetNum(Num);
		for (int32 I = 0; I < Num; ++I)
		{
			RadialDistances[I] = (LocalPoints[I] - Axis * AxialPositions[I] - PerpCenter).Length();
		}

		const float TMin = Percentile(AxialPositions, AxialTrim);
		const float TMax = Percentile(AxialPositions, 1.0f - AxialTrim);
		OutExtent = FMath::Max(0.0f, TMax - TMin);
		OutRawRadius = Settings.RadiusScale * Percentile(RadialDistances, Settings.RadiusPercentile);

		const float Radius = FMath::Max(MinRadius, FMath::Min(OutRawRadius, 0.5f * OutExtent));
		Capsule.Radius = Radius;
		Capsule.Length = FMath::Max(0.0f, OutExtent - 2.0f * Radius);
		Capsule.Center = Axis * (0.5f * (TMin + TMax)) + PerpCenter;
		return Capsule;
	}

	bool IsTrunkBoneName(const FString& Name)
	{
		const FString Lower = ToLowerAscii(Name);
		return Lower.find("hip") != FString::npos || Lower.find("pelvis") != FString::npos
			|| Lower.find("waist") != FString::npos || Lower.find("spine") != FString::npos;
	}

	// 캡슐 축은 로컬 Z(BodyInstance의 SphylBasis), Length는 양 끝 반구를 뺀 원통 길이
	FCapsuleElem FitCapsule(const TArray<FVector>& LocalPoints, const TArray<FBoneInfo>& Bones, int32 Bone, const FPhysicsAssetGenSettings& Settings)
	{
		constexpr int32 MinFitPoints = 4;
		const float MinRadius = FMath::Min(2.0f, FMath::Max(0.01f, 0.2f * Settings.MinBoneLength));

		const FVector Axis = FPhysicsBoneUtils::FindBoneAxis(Bones, Bone);
		const int32 Num = LocalPoints.Num();
		if (Num < MinFitPoints)
		{
			FCapsuleElem Capsule{};
			const FVector Z(0.0f, 0.0f, 1.0f);
			const float Dot = FMath::Clamp(Z.Dot(Axis), -1.0f, 1.0f);
			if (Dot > 0.9999f) Capsule.Rotation = FRotator();
			else if (Dot < -0.9999f) Capsule.Rotation = FQuat::MakeFromAxisAngle(FVector(1.0f, 0.0f, 0.0f), PI).ToFRotator();
			else Capsule.Rotation = FQuat::MakeFromAxisAngle(Z.Cross(Axis), std::acos(Dot)).ToFRotator();
			const float BoneLength = GetBoneLength(Bones, Bone);
			Capsule.Radius = FMath::Max(MinRadius, 0.15f * BoneLength);
			Capsule.Length = FMath::Max(0.0f, BoneLength - 2.0f * Capsule.Radius);
			Capsule.Center = Axis * (BoneLength * 0.5f);
			return Capsule;
		}

		float BestRawRadius = 0.0f;
		float BestExtent = 0.0f;
		FCapsuleElem BestCapsule = FitCapsuleAlongAxis(LocalPoints, Axis, Settings, MinRadius, BestRawRadius, BestExtent);

		// 골반이나 허리처럼 길이보다 너비가 넓은 몸통 본은 가장 긴 가로 축으로 정렬한다
		if (IsTrunkBoneName(Bones[Bone].Name) && 2.0f * BestRawRadius > BestExtent)
		{
			float BestMargin = BestExtent - 2.0f * BestRawRadius;
			const FVector Candidates[3] = { FVector(1.0f, 0.0f, 0.0f), FVector(0.0f, 1.0f, 0.0f), FVector(0.0f, 0.0f, 1.0f) };
			for (const FVector& Candidate : Candidates)
			{
				float CandRawRadius = 0.0f;
				float CandExtent = 0.0f;
				const FCapsuleElem CandCapsule = FitCapsuleAlongAxis(LocalPoints, Candidate, Settings, MinRadius, CandRawRadius, CandExtent);
				const float Margin = CandExtent - 2.0f * CandRawRadius;
				if (Margin > BestMargin)
				{
					BestMargin = Margin;
					BestCapsule = CandCapsule;
				}
			}
		}
		return BestCapsule;
	}

	FQuat ShortestRotation(const FVector& From, const FVector& To)
	{
		const float Dot = FMath::Clamp(From.Dot(To), -1.0f, 1.0f);
		if (Dot < -0.99999f)
		{
			FVector Axis = FVector(1.0f, 0.0f, 0.0f).Cross(From);
			if (Axis.SizeSquared() < 1e-6f)
			{
				Axis = FVector(0.0f, 1.0f, 0.0f).Cross(From);
			}
			return FQuat::MakeFromAxisAngle(Axis.Normalized(), PI);
		}
		if (Dot > 0.99999f)
		{
			return FQuat::Identity();
		}
		const FVector Cross = From.Cross(To);
		const float S = std::sqrt((1.0f + Dot) * 2.0f);
		return FQuat(Cross.X / S, Cross.Y / S, Cross.Z / S, S * 0.5f).Normalized();
	}

	TArray<FTransform> BuildBindTransforms(const TArray<FBoneInfo>& Bones)
	{
		TArray<FMatrix> BindMatrices;
		BindMatrices.SetNum(Bones.Num());
		TArray<FTransform> BindTransforms;
		BindTransforms.SetNum(Bones.Num());
		for (int32 B = 0; B < Bones.Num(); ++B)
		{
			const int32 Parent = Bones[B].ParentIndex;
			const FMatrix Local = Bones[B].LocalBindPose.GetLocalMatrix();
			BindMatrices[B] = Parent < 0 ? Local : Local * BindMatrices[Parent];
			BindTransforms[B] = FTransform(BindMatrices[B]);
		}
		return BindTransforms;
	}

	FVector EstimateCharacterForward(const TArray<FBoneInfo>& Bones, const TArray<FTransform>& BindTransforms)
	{
		FVector ForwardSum(0.0f, 0.0f, 0.0f);
		for (int32 B = 0; B < Bones.Num(); ++B)
		{
			const FString Lower = ToLowerAscii(Bones[B].Name);
			if (Lower.find("eye") != FString::npos && Bones[B].ParentIndex >= 0)
			{
				FVector Dir = BindTransforms[B].Location - BindTransforms[Bones[B].ParentIndex].Location;
				Dir.Z = 0.0f;
				if (Dir.SizeSquared() > 1e-4f) ForwardSum += Dir.Normalized();
			}
			else if (Lower.find("foot") != FString::npos)
			{
				for (int32 C = B + 1; C < Bones.Num(); ++C)
				{
					if (Bones[C].ParentIndex != B) continue;
					if (ToLowerAscii(Bones[C].Name).find("toe") == FString::npos) continue;
					FVector Dir = BindTransforms[C].Location - BindTransforms[B].Location;
					Dir.Z = 0.0f;
					if (Dir.SizeSquared() > 1e-4f) ForwardSum += Dir.Normalized();
					break;
				}
			}
		}
		return ForwardSum.SizeSquared() > 1e-4f ? ForwardSum.Normalized() : FVector(1.0f, 0.0f, 0.0f);
	}

	void ConfigureHingeConstraint(FPhysicsAssetConstraint& Constraint, const TArray<FBoneInfo>& Bones,
		int32 ChildBone, const TArray<FTransform>& BindTransforms, const FVector& BendTargetDir, float MaxAngleDeg)
	{
		const FVector LocalAxis = FPhysicsBoneUtils::FindBoneAxis(Bones, ChildBone);
		const FQuat JointBindRot = (BindTransforms[ChildBone].Rotation * ShortestRotation(FVector(1.0f, 0.0f, 0.0f), LocalAxis)).Normalized();
		const FVector BoneAxisWorld = JointBindRot.RotateVector(FVector(1.0f, 0.0f, 0.0f));
		const FVector HingeWorld = BoneAxisWorld.Cross(BendTargetDir);
		if (HingeWorld.SizeSquared() > 1e-4f)
		{
			const FVector HingeLocal = JointBindRot.Inverse().RotateVector(HingeWorld.Normalized());
			Constraint.FrameOffset = ShortestRotation(FVector(1.0f, 0.0f, 0.0f), HingeLocal).ToFRotator();
		}
		Constraint.Swing1Motion = EConstraintMotion::Locked;
		Constraint.Swing2Motion = EConstraintMotion::Locked;
		Constraint.TwistMotion = EConstraintMotion::Limited;
		Constraint.Swing1LimitDeg = 0.0f;
		Constraint.Swing2LimitDeg = 0.0f;
		Constraint.TwistMinDeg = -5.0f;
		Constraint.TwistMaxDeg = MaxAngleDeg;
	}

	void ConfigureDefaultConstraint(FPhysicsAssetConstraint& Constraint, const TArray<FBoneInfo>& Bones,
		int32 ChildBone, const TArray<FTransform>& BindTransforms, const FVector& CharForward)
	{
		const FString Lower = ToLowerAscii(Bones[ChildBone].Name);
		if (Lower.find("calf") != FString::npos || Lower.find("shin") != FString::npos || Lower.find("lowerleg") != FString::npos)
		{
			ConfigureHingeConstraint(Constraint, Bones, ChildBone, BindTransforms, -CharForward, 115.0f);
		}
		else if (Lower.find("forearm") != FString::npos || Lower.find("elbow") != FString::npos || Lower.find("lowerarm") != FString::npos)
		{
			ConfigureHingeConstraint(Constraint, Bones, ChildBone, BindTransforms, CharForward, 120.0f);
		}
		else if (Lower.find("spine") != FString::npos || Lower.find("waist") != FString::npos)
		{
			Constraint.Swing1LimitDeg = 20.0f;
			Constraint.Swing2LimitDeg = 20.0f;
			Constraint.TwistMinDeg = -15.0f;
			Constraint.TwistMaxDeg = 15.0f;
		}
		else if (Lower.find("head") != FString::npos || Lower.find("neck") != FString::npos)
		{
			Constraint.Swing1LimitDeg = 30.0f;
			Constraint.Swing2LimitDeg = 25.0f;
			Constraint.TwistMinDeg = -30.0f;
			Constraint.TwistMaxDeg = 30.0f;
		}
		else if (Lower.find("thigh") != FString::npos || Lower.find("upperleg") != FString::npos)
		{
			Constraint.Swing1LimitDeg = 45.0f;
			Constraint.Swing2LimitDeg = 35.0f;
			Constraint.TwistMinDeg = -25.0f;
			Constraint.TwistMaxDeg = 25.0f;
		}
		else if (Lower.find("foot") != FString::npos || Lower.find("ankle") != FString::npos)
		{
			Constraint.Swing1LimitDeg = 25.0f;
			Constraint.Swing2LimitDeg = 15.0f;
			Constraint.TwistMinDeg = -15.0f;
			Constraint.TwistMaxDeg = 15.0f;
		}
		else if (Lower.find("upperarm") != FString::npos || Lower.find("shoulder") != FString::npos)
		{
			Constraint.Swing1LimitDeg = 60.0f;
			Constraint.Swing2LimitDeg = 50.0f;
			Constraint.TwistMinDeg = -40.0f;
			Constraint.TwistMaxDeg = 40.0f;
		}
		else if (Lower.find("hand") != FString::npos || Lower.find("wrist") != FString::npos)
		{
			Constraint.Swing1LimitDeg = 35.0f;
			Constraint.Swing2LimitDeg = 25.0f;
			Constraint.TwistMinDeg = -20.0f;
			Constraint.TwistMaxDeg = 20.0f;
		}
	}
}

bool FPhysicsAssetGenInput::IsValid(FString& OutError) const
{
	if (!Bones || !BindVertices || !SkinWeights) { OutError = "PhysicsAssetGen: input arrays are missing"; return false; }
	if (Bones->IsEmpty()) { OutError = "PhysicsAssetGen: mesh has no bones"; return false; }
	if (BindVertices->Num() != SkinWeights->Num()) { OutError = "PhysicsAssetGen: vertex and skin weight counts differ"; return false; }
	for (const FSkinWeight& Weight : *SkinWeights)
	{
		for (int32 I = 0; I < MaxBoneInfluences; ++I)
		{
			if (Weight.Weight[I] > 0.0f && Weight.BoneIndex[I] >= Bones->Num()) { OutError = "PhysicsAssetGen: skin weight refers to a missing bone"; return false; }
		}
	}
	return true;
}

FPhysicsAssetGenInput FPhysicsAssetGenInput::FromImportData(const FSkeletalMeshData& Data)
{
	return { &Data.Bones, &Data.BindVertices, &Data.SkinWeight };
}

FPhysicsAssetGenInput FPhysicsAssetGenInput::FromMesh(const USkeletalMesh& Mesh)
{
	FPhysicsAssetGenInput Input;
	Input.Bones = &Mesh.GetRefSkeleton();
	const auto& LODs = Mesh.GetRenderData().LODRenderData;
	if (!LODs.IsEmpty())
	{
		Input.BindVertices = &LODs[0].BindVertices;
		Input.SkinWeights = &LODs[0].SkinWeights;
	}
	return Input;
}

bool FPhysicsAssetGenerator::Generate(UPhysicsAsset& Out, const FPhysicsAssetGenInput& In, const FPhysicsAssetGenSettings& Settings, TArray<FString>& OutWarnings)
{
	FString Error;
	if (!In.IsValid(Error))
	{
		OutWarnings.Add(Error);
		return false;
	}
	const TArray<FBoneInfo>& Bones = *In.Bones;
	const int32 NumBones = Bones.Num();

	Out.ClearBodies();
	Out.Constraints.Empty();

	TArray<int32> RolledCounts;
	RolledCounts.Init(0, NumBones);
	for (const FSkinWeight& Weight : *In.SkinWeights)
	{
		const int32 Dominant = FindDominantBone(Weight, Settings.MinWeight);
		if (Dominant >= 0) ++RolledCounts[Dominant];
	}

	const TArray<float> VertexExtents = ComputeBoneVertexExtents(In, Settings.MinWeight);
	TArray<uint8> HasBody;
	HasBody.Init(0, NumBones);
	// 제외 본이나 너무 짧은 본의 정점은 부모 구조 본으로 합산해 누락을 막는다
	for (int32 B = NumBones - 1; B >= 0; --B)
	{
		const float StructuralExtent = FMath::Max(GetBoneLength(Bones, B), VertexExtents[B]);
		const bool bQualifies = !IsExcludedBoneName(Bones[B].Name)
			&& StructuralExtent >= Settings.MinBoneLength
			&& RolledCounts[B] >= Settings.MinVertexCount;
		if (bQualifies)
		{
			HasBody[B] = 1;
		}
		else if (Bones[B].ParentIndex >= 0)
		{
			RolledCounts[Bones[B].ParentIndex] += RolledCounts[B];
		}
	}

	int32 NumRoots = 0;
	int32 CommonAncestor = -1;
	for (int32 B = 0; B < NumBones && (NumRoots == 0 || CommonAncestor >= 0); ++B)
	{
		if (!HasBody[B] || FindNearestBodyAncestor(Bones, HasBody, B) >= 0) continue;
		if (NumRoots++ == 0) { CommonAncestor = B; continue; }

		int32 A = CommonAncestor;
		int32 C = B;
		while (A != C && A >= 0 && C >= 0)
		{
			if (A > C) A = Bones[A].ParentIndex;
			else C = Bones[C].ParentIndex;
		}
		CommonAncestor = A == C ? A : -1;
	}
	if (NumRoots > 1)
	{
		if (CommonAncestor >= 0)
		{
			HasBody[CommonAncestor] = 1;
			// 공통 루트 바로 아래 겹치는 자식 본은 루트로 합쳐 중복 바디를 피한다
			for (int32 B = CommonAncestor + 1; B < NumBones; ++B)
			{
				if (HasBody[B] && Bones[B].ParentIndex == CommonAncestor
					&& Bones[B].LocalBindPose.Location.Length() < Settings.MinBoneLength * 0.5f)
				{
					HasBody[B] = 0;
				}
			}
			OutWarnings.Add("PhysicsAssetGen: multiple root bodies; added a body to common ancestor '" + Bones[CommonAncestor].Name + "'");
		}
		else
		{
			OutWarnings.Add("PhysicsAssetGen: root bodies have no common ancestor bone; the skeleton has several root bones");
		}
	}

	const TArray<TArray<FVector>> Points = GatherBodyPoints(In, HasBody, Settings.MinWeight);
	for (int32 B = 0; B < NumBones; ++B)
	{
		if (!HasBody[B]) continue;
		UBodySetup* Body = NewObject<UBodySetup>(&Out);
		Body->BoneName = Bones[B].Name;
		Body->AggGeom.CapsuleElems.Add(FitCapsule(Points[B], Bones, B, Settings));
		Out.BodySetups.Add(Body);
	}

	// 빈 에셋이 저장되면 다음 Import부터 자동 생성을 건너뛴다.
	if (Out.BodySetups.IsEmpty())
	{
		OutWarnings.Add("PhysicsAssetGen: no bone qualified for a body");
		return false;
	}

	if (Settings.bCreateConstraints)
	{
		RebuildConstraints(Out, Bones);
		if (!Out.Validate(Bones, "PhysicsAssetGen"))
		{
			OutWarnings.Add("PhysicsAssetGen: generated asset failed validation (see [PhysicsAsset] log)");
			return false;
		}
	}
	return true;
}

UBodySetup* FPhysicsAssetGenerator::GenerateBodyForBone(UPhysicsAsset& Asset, const FPhysicsAssetGenInput& In, int32 BoneIndex, const FPhysicsAssetGenSettings& Settings)
{
	FString Error;
	if (!In.IsValid(Error) || BoneIndex < 0 || BoneIndex >= In.Bones->Num()) return nullptr;
	const TArray<FBoneInfo>& Bones = *In.Bones;

	TArray<uint8> HasBody = GetBodyBonesFromAsset(Asset, Bones);
	HasBody[BoneIndex] = 1;
	const TArray<TArray<FVector>> Points = GatherBodyPoints(In, HasBody, Settings.MinWeight);
	const FCapsuleElem Capsule = FitCapsule(Points[BoneIndex], Bones, BoneIndex, Settings);

	UBodySetup* Body = nullptr;
	for (UBodySetup* Existing : Asset.BodySetups)
	{
		if (Existing && Existing->BoneName == Bones[BoneIndex].Name) { Body = Existing; break; }
	}
	if (!Body)
	{
		Body = NewObject<UBodySetup>(&Asset);
		Body->BoneName = Bones[BoneIndex].Name;
		Asset.BodySetups.Add(Body);
	}
	Body->AggGeom.EmptyElements();
	Body->AggGeom.CapsuleElems.Add(Capsule);
	return Body;
}

void FPhysicsAssetGenerator::RebuildConstraints(UPhysicsAsset& Asset, const TArray<FBoneInfo>& Bones)
{
	const TArray<uint8> HasBody = GetBodyBonesFromAsset(Asset, Bones);
	const TArray<FPhysicsAssetConstraint> Previous = Asset.Constraints;
	Asset.Constraints.Empty();

	const TArray<FTransform> BindTransforms = BuildBindTransforms(Bones);
	const FVector CharForward = EstimateCharacterForward(Bones, BindTransforms);

	for (int32 B = 0; B < Bones.Num(); ++B)
	{
		if (!HasBody[B]) continue;
		const int32 Parent = FindNearestBodyAncestor(Bones, HasBody, B);
		if (Parent < 0) continue;

		FPhysicsAssetConstraint Constraint;
		bool bHadPrevious = false;
		for (const FPhysicsAssetConstraint& Old : Previous)
		{
			if (Old.ChildBone == Bones[B].Name)
			{
				Constraint = Old;
				bHadPrevious = true;
				break;
			}
		}
		if (!bHadPrevious)
		{
			ConfigureDefaultConstraint(Constraint, Bones, B, BindTransforms, CharForward);
		}
		Constraint.ChildBone = Bones[B].Name;
		Constraint.ParentBone = Bones[Parent].Name;
		Asset.Constraints.Add(Constraint);
	}
}

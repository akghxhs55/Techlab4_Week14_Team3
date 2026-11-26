#pragma once

#include "AnimNodeBase.h"
#include "AnimationRuntime.h"
#include <algorithm>

struct FAnimNode_LayeredBoneBlend : public FAnimNode_Base
{
	FPoseLink BasePose;
	FPoseLink BlendPose;
	FString StartBoneName = "spine_01";
	float Alpha = 1.0f;
	bool bMeshSpaceRotationBlend = false;

	void Initialize(const FAnimationInitializeContext& Context) override
	{
		BasePose.Initialize(Context);
		BlendPose.Initialize(Context);
	}

	void Update(const FAnimationUpdateContext& Context) override
	{
		BasePose.Update(Context);
		const float Weight = std::clamp(Alpha, 0.0f, 1.0f);
		if (Weight > 0.0f)
		{
			BlendPose.Update(Context.FractionalWeight(Weight));
		}
		else
		{
			BlendPose.Update(Context.FractionalWeight(0.0f));
		}
	}

	void Evaluate(FPoseContext& Output) override
	{
		BasePose.Evaluate(Output);
		const float Weight = std::clamp(Alpha, 0.0f, 1.0f);
		if (Weight <= 0.0f || StartBoneName.empty())
			return;

		const TArray<FBoneInfo>& RefSkeleton = Output.GetRefSkeleton();
		const int32 NumBones = RefSkeleton.Num();
		int32 StartBoneIndex = -1;
		for (int32 BoneIdx = 0; BoneIdx < NumBones; ++BoneIdx)
		{
			if (RefSkeleton[BoneIdx].Name == StartBoneName)
			{
				StartBoneIndex = BoneIdx;
				break;
			}
		}
		if (StartBoneIndex < 0)
			return;

		FPoseContext BlendContext(Output);
		BlendPose.Evaluate(BlendContext);

		TArray<uint8> BranchMask;
		BranchMask.SetNum(NumBones);
		for (int32 BoneIdx = 0; BoneIdx < NumBones; ++BoneIdx)
			BranchMask[BoneIdx] = 0;
		BranchMask[StartBoneIndex] = 1;
		for (int32 BoneIdx = 0; BoneIdx < NumBones; ++BoneIdx)
		{
			const int32 ParentIdx = RefSkeleton[BoneIdx].ParentIndex;
			if (ParentIdx >= 0 && ParentIdx < NumBones && BranchMask[ParentIdx] != 0)
			{
				BranchMask[BoneIdx] = 1;
			}
		}

		TArray<FQuat> BaseCSRot;
		TArray<FQuat> BlendCSRot;
		TArray<FQuat> ResultCSRot;
		if (bMeshSpaceRotationBlend)
		{
			BaseCSRot.SetNum(NumBones);
			BlendCSRot.SetNum(NumBones);
			ResultCSRot.SetNum(NumBones);
			for (int32 BoneIdx = 0; BoneIdx < NumBones; ++BoneIdx)
			{
				const int32 ParentIdx = RefSkeleton[BoneIdx].ParentIndex;
				const FQuat BaseLocal = Output.Pose[BoneIdx].Rotation.Normalized();
				const FQuat BlendLocal = BlendContext.Pose[BoneIdx].Rotation.Normalized();
				BaseCSRot[BoneIdx] = (ParentIdx >= 0 && ParentIdx < BoneIdx)
					? (BaseCSRot[ParentIdx] * BaseLocal).Normalized() : BaseLocal;
				BlendCSRot[BoneIdx] = (ParentIdx >= 0 && ParentIdx < BoneIdx)
					? (BlendCSRot[ParentIdx] * BlendLocal).Normalized() : BlendLocal;
			}
		}

		for (int32 BoneIdx = 0; BoneIdx < NumBones; ++BoneIdx)
		{
			if (BranchMask[BoneIdx] == 0)
			{
				if (bMeshSpaceRotationBlend)
					ResultCSRot[BoneIdx] = BaseCSRot[BoneIdx];
				continue;
			}

			Output.Pose[BoneIdx].Location = FMath::Lerp(
				Output.Pose[BoneIdx].Location, BlendContext.Pose[BoneIdx].Location, Weight);
			Output.Pose[BoneIdx].Scale = FMath::Lerp(
				Output.Pose[BoneIdx].Scale, BlendContext.Pose[BoneIdx].Scale, Weight);

			if (!bMeshSpaceRotationBlend)
			{
				Output.Pose[BoneIdx].Rotation = FMath::Slerp(
					Output.Pose[BoneIdx].Rotation, BlendContext.Pose[BoneIdx].Rotation, Weight);
				continue;
			}

			const int32 ParentIdx = RefSkeleton[BoneIdx].ParentIndex;
			const FQuat BlendedCS = FMath::Slerp(BaseCSRot[BoneIdx], BlendCSRot[BoneIdx], Weight).Normalized();
			Output.Pose[BoneIdx].Rotation = (ParentIdx >= 0 && ParentIdx < BoneIdx)
				? (ResultCSRot[ParentIdx].Inverse() * BlendedCS).Normalized() : BlendedCS;
			ResultCSRot[BoneIdx] = BlendedCS;
		}
	}
};

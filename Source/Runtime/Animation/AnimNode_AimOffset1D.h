#pragma once

#include "AnimNodeBase.h"
#include "AnimSequence.h"
#include "AnimMontageAsset.h"
#include "AnimationRuntime.h"
#include <algorithm>
#include <cmath>

struct FAnimNode_AimOffset1D : public FAnimNode_Base
{
	struct FSampleEntry
	{
		float Pitch = 0.0f;
		UAnimSequence* Sequence = nullptr;
		int32 FrameIndex = 0;
	};

	FPoseLink BasePose;
	FAimOffsetAsset Asset;
	UAnimSequence* BaseSequence = nullptr;
	TArray<FSampleEntry> Samples;
	float Pitch = 0.0f;
	float Alpha = 1.0f;

	void Initialize(const FAnimationInitializeContext& Context) override
	{
		BasePose.Initialize(Context);
	}

	void Update(const FAnimationUpdateContext& Context) override
	{
		BasePose.Update(Context);
	}

	static bool SampleSequenceFrame(UAnimSequence* Seq, int32 FrameIndex, FPoseContext& OutPose)
	{
		if (!Seq || !Seq->GetDataModel())
			return false;
		const UAnimDataModel* Model = Seq->GetDataModel();
		const int32 NumKeys = Model->GetNumberOfKeys();
		float Time = 0.0f;
		if (NumKeys > 1)
		{
			const int32 ClampedFrame = std::clamp(FrameIndex, 0, NumKeys - 1);
			Time = (static_cast<float>(ClampedFrame) / static_cast<float>(NumKeys - 1)) * Model->GetPlayLength();
		}
		Seq->GetAnimationPose(Time, OutPose.Pose, OutPose.GetRefSkeleton(), false);
		return true;
	}

	bool SamplePoseAtPitch(float SamplePitch, FPoseContext& OutPose) const
	{
		if (Samples.IsEmpty())
			return false;
		const float Clamped = std::clamp(SamplePitch, Asset.MinPitch, Asset.MaxPitch);
		if (Samples.Num() == 1 || Clamped <= Samples[0].Pitch)
			return SampleSequenceFrame(Samples[0].Sequence, Samples[0].FrameIndex, OutPose);

		const int32 LastIdx = Samples.Num() - 1;
		if (Clamped >= Samples[LastIdx].Pitch)
			return SampleSequenceFrame(Samples[LastIdx].Sequence, Samples[LastIdx].FrameIndex, OutPose);

		for (int32 Idx = 0; Idx < LastIdx; ++Idx)
		{
			const FSampleEntry& Low = Samples[Idx];
			const FSampleEntry& High = Samples[Idx + 1];
			if (Clamped >= Low.Pitch && Clamped <= High.Pitch)
			{
				const float Span = High.Pitch - Low.Pitch;
				if (Span <= 1e-5f)
					return SampleSequenceFrame(Low.Sequence, Low.FrameIndex, OutPose);

				FPoseContext PoseLow(OutPose);
				FPoseContext PoseHigh(OutPose);
				if (!SampleSequenceFrame(Low.Sequence, Low.FrameIndex, PoseLow) ||
					!SampleSequenceFrame(High.Sequence, High.FrameIndex, PoseHigh))
				{
					return false;
				}
				const float BlendWeight = std::clamp((Clamped - Low.Pitch) / Span, 0.0f, 1.0f);
				FAnimationRuntime::BlendTwoPosesTogether(
					PoseLow.Pose, PoseHigh.Pose, PoseLow.Curve, PoseHigh.Curve,
					1.0f - BlendWeight, OutPose.Pose, OutPose.Curve);
				return true;
			}
		}
		return SampleSequenceFrame(Samples[LastIdx].Sequence, Samples[LastIdx].FrameIndex, OutPose);
	}

	bool EvaluateReferencePose(FPoseContext& OutPose) const
	{
		if (BaseSequence && SampleSequenceFrame(BaseSequence, Asset.BaseFrameIndex, OutPose))
			return true;
		return SamplePoseAtPitch(0.0f, OutPose);
	}

	void Evaluate(FPoseContext& Output) override
	{
		BasePose.Evaluate(Output);
		const float Weight = std::clamp(Alpha, 0.0f, 1.0f);
		if (Weight <= 0.0f || Samples.IsEmpty())
			return;

		FPoseContext RefPose(Output);
		FPoseContext AimPose(Output);
		if (!EvaluateReferencePose(RefPose) || !SamplePoseAtPitch(Pitch, AimPose))
			return;

		const TArray<FBoneInfo>& RefSkeleton = Output.GetRefSkeleton();
		const int32 NumBones = RefSkeleton.Num();
		if (Output.Pose.GetNumBones() != NumBones || RefPose.Pose.GetNumBones() != NumBones || AimPose.Pose.GetNumBones() != NumBones)
			return;

		TArray<FQuat> BaseCSRot;
		TArray<FQuat> RefCSRot;
		TArray<FQuat> AimCSRot;
		TArray<FQuat> NewCSRot;
		BaseCSRot.SetNum(NumBones);
		RefCSRot.SetNum(NumBones);
		AimCSRot.SetNum(NumBones);
		NewCSRot.SetNum(NumBones);

		for (int32 BoneIdx = 0; BoneIdx < NumBones; ++BoneIdx)
		{
			const int32 ParentIdx = RefSkeleton[BoneIdx].ParentIndex;
			const FQuat BaseLocal = Output.Pose[BoneIdx].Rotation.Normalized();
			const FQuat RefLocal = RefPose.Pose[BoneIdx].Rotation.Normalized();
			const FQuat AimLocal = AimPose.Pose[BoneIdx].Rotation.Normalized();
			if (ParentIdx >= 0 && ParentIdx < BoneIdx)
			{
				BaseCSRot[BoneIdx] = (BaseCSRot[ParentIdx] * BaseLocal).Normalized();
				RefCSRot[BoneIdx] = (RefCSRot[ParentIdx] * RefLocal).Normalized();
				AimCSRot[BoneIdx] = (AimCSRot[ParentIdx] * AimLocal).Normalized();
			}
			else
			{
				BaseCSRot[BoneIdx] = BaseLocal;
				RefCSRot[BoneIdx] = RefLocal;
				AimCSRot[BoneIdx] = AimLocal;
			}

			const FQuat FullDeltaCS = (AimCSRot[BoneIdx] * RefCSRot[BoneIdx].Inverse()).Normalized();
			const FQuat WeightedDeltaCS = FMath::Slerp(FQuat::Identity(), FullDeltaCS, Weight);
			NewCSRot[BoneIdx] = (WeightedDeltaCS * BaseCSRot[BoneIdx]).Normalized();
		}

		for (int32 BoneIdx = 0; BoneIdx < NumBones; ++BoneIdx)
		{
			const int32 ParentIdx = RefSkeleton[BoneIdx].ParentIndex;
			if (ParentIdx >= 0 && ParentIdx < BoneIdx)
			{
				Output.Pose[BoneIdx].Rotation = (NewCSRot[ParentIdx].Inverse() * NewCSRot[BoneIdx]).Normalized();
			}
			else
			{
				Output.Pose[BoneIdx].Rotation = NewCSRot[BoneIdx];
			}
		}
	}
};

#pragma once

#include "AnimNodeBase.h"
#include "AnimationRuntime.h"

struct FAnimNode_TwoWayBlend : public FAnimNode_Base
{
	FPoseLink A;
	FPoseLink B;

	float Alpha = 0.0f;
	bool bSyncByNormalizedTime = false;

	void Initialize(const FAnimationInitializeContext& Context) override
	{
		A.Initialize(Context);
		B.Initialize(Context);
	}

	virtual void Update(const FAnimationUpdateContext& Context) override
	{
		const float ClampedAlpha = FMath::Clamp(Alpha, 0.0f, 1.0f);
		FAnimNode_Base* NodeA = A.GetLinkNode();
		FAnimNode_Base* NodeB = B.GetLinkNode();
		if (!bSyncByNormalizedTime || !NodeA || !NodeB)
		{
			A.Update(Context.FractionalWeight(1.f - ClampedAlpha));
			B.Update(Context.FractionalWeight(ClampedAlpha));
			return;
		}

		const bool bBIsLeader = ClampedAlpha >= 0.5f;
		FAnimNode_Base* Leader = bBIsLeader ? NodeB : NodeA;
		FAnimNode_Base* Follower = bBIsLeader ? NodeA : NodeB;
		const float LeaderWeight = bBIsLeader ? ClampedAlpha : (1.0f - ClampedAlpha);
		const float FollowerWeight = 1.0f - LeaderWeight;

		Leader->Update(Context.FractionalWeight(LeaderWeight));
		const float LeaderCycle = Leader->GetCycleSeconds();
		const float FollowerCycle = Follower->GetCycleSeconds();
		float LeaderTime = 0.0f;
		if (Leader->IsLoopingNode() && Follower->IsLoopingNode() &&
			LeaderCycle > 0.0f && FollowerCycle > 0.0f && Leader->GetNormalizedTime(LeaderTime))
		{
			const float ScaledDt = Context.GetDeltaTime() * FollowerCycle / LeaderCycle;
			const FAnimationUpdateContext SyncedContext(Context.GetAnimInstance<UAnimInstance>(), ScaledDt);
			Follower->Update(SyncedContext.FractionalWeight(FollowerWeight));
			Follower->SetNormalizedTime(LeaderTime);
		}
		else
		{
			Follower->Update(Context.FractionalWeight(FollowerWeight));
		}
	}

	virtual void Evaluate(FPoseContext& Output) override
	{
		FPoseContext PoseA(Output);
		FPoseContext PoseB(Output);

		A.Evaluate(PoseA);
		B.Evaluate(PoseB);

		const float ClampedAlpha = FMath::Clamp(Alpha, 0.0f, 1.0f);

		FAnimationRuntime::BlendTwoPosesTogether(
			PoseA.Pose, PoseB.Pose, PoseA.Curve, PoseB.Curve,
			1.0f - ClampedAlpha,
			Output.Pose, Output.Curve);
	}
};

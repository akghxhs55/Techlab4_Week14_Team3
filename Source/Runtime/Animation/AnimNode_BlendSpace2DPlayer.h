#pragma once

#include "AnimNodeBase.h"
#include "BlendSpace2D.h"
#include "AnimationRuntime.h"
#include "AnimInstance.h"
#include "AnimNotifyQueue.h"
#include <array>
#include <cmath>

struct FAnimNode_BlendSpace2DPlayer : public FAnimNode_Base
{
	UBlendSpace2D* BlendSpace = nullptr;

	float X = 0.0f;
	float Y = 0.0f;
	float PlayRate = 1.0f;
	float Phase = 0.0f;
	bool bLooping = true;

	void Initialize(const FAnimationInitializeContext& Context) override
	{
		Phase = 0.0f;
	}

	void Update(const FAnimationUpdateContext& Context) override
	{
		if (!BlendSpace)
		{
			return;
		}

		std::array<FWeightedBlendSample, 3> Blend;
		int32 Count = 0;
		if (!BlendSpace->GetBlendSamples(X, Y, Blend, Count))
		{
			return;
		}

		const float EffectiveLength = BlendSpace->GetEffectiveLength(X, Y);
		if (EffectiveLength <= 0.0f || !std::isfinite(EffectiveLength))
		{
			Phase = 0.0f;
			return;
		}

		const float PrevPhase = Phase;
		const float DeltaPhase = Context.GetDeltaTime() * PlayRate / EffectiveLength;

		Phase += DeltaPhase;
		if (!bLooping)
		{
			Phase = FMath::Clamp(Phase, 0.0f, 1.0f);
		}
		else
		{
			Phase = std::fmod(Phase, 1.0f);
			if (Phase < 0.0f)
			{
				Phase += 1.0f;
			}
		}

		UAnimInstance* AnimInstance = Context.GetAnimInstance<UAnimInstance>();
		if (!AnimInstance)
		{
			return;
		}

		const float FinalWeight = Context.GetFinalBlendWeight();
		for (int32 I = 0; I < Count; ++I)
		{
			if (Blend[I].Weight <= 0.0f || !Blend[I].Sample || !Blend[I].Sample->Animation)
			{
				continue;
			}
			const UAnimSequence* Sequence = Blend[I].Sample->Animation;
			if (!Sequence->GetDataModel())
			{
				continue;
			}
			const float Length = Sequence->GetDataModel()->GetPlayLength();
			FAnimNotifyContext NotifyContext(bLooping, Sequence);
			Sequence->GetAnimNotifies(PrevPhase * Length, DeltaPhase * Length, NotifyContext);
			AnimInstance->AddNotifies(NotifyContext.ActiveNotifies, FinalWeight * Blend[I].Weight);
		}
	}

	void Evaluate(FPoseContext& Output) override
	{
		if (!BlendSpace)
		{
			Output.ResetToRefPose();
			return;
		}

		const UAnimInstance* AnimInstance = Output.GetAnimInstance<UAnimInstance>();
		const bool bExtractRootMotion = !AnimInstance || AnimInstance->ShouldExtractRootMotion();
		BlendSpace->EvaluatePose(X, Y, Phase, Output, bExtractRootMotion);
	}

	float GetCycleSeconds() const override
	{
		if (!BlendSpace || PlayRate <= 0.0f || !std::isfinite(PlayRate))
		{
			return 0.0f;
		}
		const float EffectiveLength = BlendSpace->GetEffectiveLength(X, Y);
		return EffectiveLength > 0.0f ? (EffectiveLength / PlayRate) : 0.0f;
	}

	bool GetNormalizedTime(float& OutTime) const override
	{
		if (!BlendSpace)
		{
			return false;
		}
		OutTime = FMath::Clamp(Phase, 0.0f, 1.0f);
		return true;
	}

	void SetNormalizedTime(float Time) override
	{
		if (std::isfinite(Time))
		{
			Phase = FMath::Clamp(Time, 0.0f, 1.0f);
		}
	}

	bool IsLoopingNode() const override { return bLooping; }
};

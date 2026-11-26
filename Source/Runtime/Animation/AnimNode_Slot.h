#pragma once

#include "AnimNodeBase.h"

class UAnimGraphInstance;

struct FAnimNode_Slot : public FAnimNode_Base
{
	FPoseLink Source;
	FString SlotName = "UpperBody";
	UAnimGraphInstance* GraphInstance = nullptr;

	void Initialize(const FAnimationInitializeContext& Context) override
	{
		Source.Initialize(Context);
	}

	void Update(const FAnimationUpdateContext& Context) override
	{
		Source.Update(Context);
	}

	// AnimGraphInstance.cpp에 정의해 순환 include를 방지
	void Evaluate(FPoseContext& Output) override;
};

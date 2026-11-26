#include "EnginePCH.h"
#include "ConstraintInstance.h"

#include <PxPhysics.h>
#include <extensions/PxD6Joint.h>
#include <extensions/PxJointLimit.h>

#include "BodyInstance.h"
#include "PhysicsCore.h"
#include "PhysXConversion.h"

using namespace physx;

namespace
{
	PxD6Motion::Enum ConvertMotion(EConstraintMotion InMotion)
	{
		switch (InMotion)
		{
		case EConstraintMotion::Locked:
			return PxD6Motion::eLOCKED;
		case EConstraintMotion::Limited:
			return PxD6Motion::eLIMITED;
		case EConstraintMotion::Free:
			return PxD6Motion::eFREE;
		default:
			return PxD6Motion::eLOCKED;
		}
	}
}

void FConstraintInstance::InitConstraint(
	FBodyInstance* InBody1, 
	FBodyInstance* InBody2, 
	const FTransform& InFrame1, 
	const FTransform& InFrame2, 
	FPhysScene* InScene)
{
	if (Joint)
	{
		TermConstraint();
	}

	ConstraintBone1 = InBody1;
	ConstraintBone2 = InBody2;
	RefFrame1 = InFrame1;
	RefFrame2 = InFrame2;

	PxRigidActor* Actor1 = InBody1 ? InBody1->RigidActor : nullptr;
	PxRigidActor* Actor2 = InBody2 ? InBody2->RigidActor : nullptr;

	if (!Actor1 && !Actor2)
	{
		KRS_LOG(Warning, "[Physics] InitConstraint failed: Both bodies are null");
		return;
	}

	if (Actor1 && Actor2 && Actor1 == Actor2)
	{
		KRS_LOG(Warning, "[Physics] InitConstraint failed: Both bodies refer to the same actor");
		return;
	}

	PxPhysics* Physics = FPhysicsCore::GetPhysics();
	if (!Physics)
	{
		KRS_LOG(Error, "[Physics] InitConstraint failed: Physics SDK is null");
		return;
	}

	const PxTransform Pose1 = K2PTransform(InFrame1);
	const PxTransform Pose2 = K2PTransform(InFrame2);

	Joint = PxD6JointCreate(*Physics, Actor1, Pose1, Actor2, Pose2);
	if (!Joint)
	{
		KRS_LOG(Error, "[Physics] PxD6JointCreate failed");
		return;
	}

	UpdateLinearLimit();
	UpdateAngularLimit();
	UpdateBreakable();

	Joint->setConstraintFlag(PxConstraintFlag::eCOLLISION_ENABLED, bEnableCollision);

	Joint->userData = this;
}

void FConstraintInstance::TermConstraint()
{
	if (Joint)
	{
		Joint->userData = nullptr;
		Joint->release();
		Joint = nullptr;
	}
	ConstraintBone1 = nullptr;
	ConstraintBone2 = nullptr;
}

void FConstraintInstance::UpdateLinearLimit()
{
	if (!Joint)
	{
		return;
	}

	Joint->setMotion(PxD6Axis::eX, ConvertMotion(LinearX));
	Joint->setMotion(PxD6Axis::eY, ConvertMotion(LinearY));
	Joint->setMotion(PxD6Axis::eZ, ConvertMotion(LinearZ));

	const bool bHasLinearLimit = (LinearX == EConstraintMotion::Limited)
		|| (LinearY == EConstraintMotion::Limited)
		|| (LinearZ == EConstraintMotion::Limited);

	if (bHasLinearLimit && LinearLimitDistance > 0.0f)
	{
		Joint->setDistanceLimit(PxJointLinearLimit(LinearLimitDistance));
	}
}

void FConstraintInstance::UpdateAngularLimit()
{
	if (!Joint)
	{
		return;
	}

	Joint->setMotion(PxD6Axis::eSWING1, ConvertMotion(Swing1Motion));
	Joint->setMotion(PxD6Axis::eSWING2, ConvertMotion(Swing2Motion));
	Joint->setMotion(PxD6Axis::eTWIST, ConvertMotion(TwistMotion));

	// Swing1 / Swing2 제한 (Cone 형태)
	const bool bHasSwingLimit = (Swing1Motion == EConstraintMotion::Limited)
		|| (Swing2Motion == EConstraintMotion::Limited);

	if (bHasSwingLimit)
	{
		const float Swing1Rad = FMath::Clamp(FMath::DegreesToRadians(Swing1LimitAngle), 0.001f, PI - 0.001f);
		const float Swing2Rad = FMath::Clamp(FMath::DegreesToRadians(Swing2LimitAngle), 0.001f, PI - 0.001f);
		Joint->setSwingLimit(PxJointLimitCone(Swing1Rad, Swing2Rad));
	}

	// Twist 제한 (AngularLimitPair)
	if (TwistMotion == EConstraintMotion::Limited)
	{
		float LowerRad = FMath::DegreesToRadians(TwistLimitLowerAngle);
		float UpperRad = FMath::DegreesToRadians(TwistLimitUpperAngle);
		if (LowerRad > UpperRad)
		{
			std::swap(LowerRad, UpperRad);
		}
		LowerRad = FMath::Clamp(LowerRad, -2.0f * PI + 0.001f, 2.0f * PI - 0.001f);
		UpperRad = FMath::Clamp(UpperRad, -2.0f * PI + 0.001f, 2.0f * PI - 0.001f);
		Joint->setTwistLimit(PxJointAngularLimitPair(LowerRad, UpperRad));
	}
}

void FConstraintInstance::UpdateBreakable()
{
	if (!Joint)
	{
		return;
	}

	Joint->setBreakForce(BreakForce, BreakTorque);
}

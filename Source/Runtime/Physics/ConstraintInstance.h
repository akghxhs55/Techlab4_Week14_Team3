#pragma once

#include "Math/Transform.h"
#include "ConstraintInstance.generated.h"

struct FBodyInstance;
class FPhysScene;

namespace physx
{
	class PxD6Joint;
}

UENUM()
enum class EConstraintMotion : uint8
{
	Locked,
	Limited,
	Free
};

USTRUCT()
struct FConstraintInstance
{
	GENERATED_BODY()

	// ---- 선형(이동) 구속 ----
	UPROPERTY(EditAnywhere)
	EConstraintMotion LinearX = EConstraintMotion::Locked;

	UPROPERTY(EditAnywhere)
	EConstraintMotion LinearY = EConstraintMotion::Locked;

	UPROPERTY(EditAnywhere)
	EConstraintMotion LinearZ = EConstraintMotion::Locked;

	UPROPERTY(EditAnywhere)
	float LinearLimitDistance = 0.0f;

	// ---- 각운동(회전) 구속 ----
	// Swing1: Y축 회전, Swing2: Z축 회전
	UPROPERTY(EditAnywhere)
	EConstraintMotion Swing1Motion = EConstraintMotion::Limited;

	UPROPERTY(EditAnywhere)
	EConstraintMotion Swing2Motion = EConstraintMotion::Limited;

	// Twist: X축(비틀림) 회전
	UPROPERTY(EditAnywhere)
	EConstraintMotion TwistMotion = EConstraintMotion::Limited;

	// 회전 제한 각도 (도 단위)
	UPROPERTY(EditAnywhere)
	float Swing1LimitAngle = 45.0f;

	UPROPERTY(EditAnywhere)
	float Swing2LimitAngle = 45.0f;

	UPROPERTY(EditAnywhere)
	float TwistLimitLowerAngle = -45.0f;

	UPROPERTY(EditAnywhere)
	float TwistLimitUpperAngle = 45.0f;

	// ---- 조인트 파괴 및 프로젝션(늘어남 방지) ----
	UPROPERTY(EditAnywhere)
	bool bEnableCollision = false;

	UPROPERTY(EditAnywhere)
	bool bEnableProjection = true;

	UPROPERTY(EditAnywhere)
	float ProjectionLinearTolerance = 0.1f;

	UPROPERTY(EditAnywhere)
	float ProjectionAngularTolerance = 10.0f;

	UPROPERTY(EditAnywhere)
	float BreakForce = 3.402823466e+38F;

	UPROPERTY(EditAnywhere)
	float BreakTorque = 3.402823466e+38F;

	// ---- 생명주기 관리 ----
	~FConstraintInstance() { TermConstraint(); }
	void InitConstraint(FBodyInstance* InBody1, FBodyInstance* InBody2, const FTransform& InFrame1, const FTransform& InFrame2, FPhysScene* InScene = nullptr);
	void TermConstraint();

	// ---- 속성 갱신 ----
	void UpdateLinearLimit();
	void UpdateAngularLimit();
	void UpdateBreakable();

	// ---- 런타임 바인딩 정보 ----
	FBodyInstance* ConstraintBone1 = nullptr; // 부모 뼈 바디
	FBodyInstance* ConstraintBone2 = nullptr; // 자식 뼈 바디

	FTransform RefFrame1; // Body1 기준 조인트 로컬 트랜스폼
	FTransform RefFrame2; // Body2 기준 조인트 로컬 트랜스폼

	physx::PxD6Joint* Joint = nullptr;

	// ---- 유틸리티 ----
	bool IsValidConstraint() const { return Joint != nullptr; }
	physx::PxD6Joint* GetPxD6Joint() const { return Joint; }
};

#pragma once

#include "Math/Vector.h"
#include "Math/Transform.h"
#include "VehicleParams.h"

namespace physx
{
	class PxRigidDynamic;
	class PxScene;
}

// 개별 바퀴의 런타임 물리 시뮬레이션 상태
struct FWheelSimulationState
{
	bool bInContact = false;                    // 지면 접촉 여부
	float SuspensionJounce = 0.0f;              // 서스펜션 압축량 (m)
	float SteerAngle = 0.0f;                    // 현재 조향각 (도 단위)
	float RotationSpeed = 0.0f;                 // 바퀴 회전 각속도 (rad/s)
	float RotationAngle = 0.0f;                 // 바퀴 누적 회전각 (rad, 렌더링용)
	float SlipRatio = 0.0f;                     // 종방향 슬립비 (휠 스핀 / 브레이크 락 지표)
	float SlipAngleRad = 0.0f;                  // 횡방향 슬립각 (rad)
	bool bOnGravel = false;                     // 코스 이탈 자갈/비포장 노면 접촉 여부
	FTransform LocalPose = FTransform::Identity;// 차체 기준 휠 렌더링 로컬 트랜스폼
	FVector ContactPoint = FVector::ZeroVector; // 지면 접점
	FVector ContactNormal = FVector(0.0f, 0.0f, 1.0f); // 지면 법선
};

// 차량 시뮬레이션 파이프라인 (지면 레이캐스트, 서스펜션, 타이어 마찰, 차체 힘 인가)
class FVehicleSimulationPipeline
{
public:
	FVehicleSimulationPipeline();
	~FVehicleSimulationPipeline();

	// 파라미터 및 대상 액터 초기화
	void Initialize(const FVehicleParams& InParams, physx::PxRigidDynamic* InChassisActor);

	// 매 물리 틱 업데이트 (서스펜션 레이캐스트 -> 반발력/타이어 마찰 계산 -> 차체 힘 인가)
	void Update(float DeltaTime, physx::PxScene* Scene, float Throttle, float Steer, float Brake, bool bHandbrake);

	// 결과 조회
	const FWheelSimulationState& GetWheelState(EVehicleWheelOrder Order) const;
	const FWheelSimulationState& GetWheelState(int32 Index) const;

	float GetForwardSpeed() const; // 전진 속도 (m/s)
	float GetForwardSpeedKmh() const { return GetForwardSpeed() * 3.6f; } // 전진 속도 (km/h)

	// 파워트레인 및 주행 텔레메트리 조회
	int32 GetCurrentGear() const { return CurrentGear; }
	float GetEngineRPM() const { return EngineRPM; }
	float GetMaxRPM() const { return Params.MaxRPM; }
	float GetLateralG() const { return LateralG; }
	float GetLongitudinalG() const { return LongitudinalG; }
	float GetAverageRearSlipRatio() const;
	bool IsTCActive() const { return bTCActive; }

	void SetTractionControlEnabled(bool bEnable) { bTractionControlEnabled = bEnable; }
	bool IsTractionControlEnabled() const { return bTractionControlEnabled; }

	void SetDRSRequested(bool bRequest) { bDRSRequested = bRequest; }
	bool IsDRSActive() const { return bDRSActive; }
	bool IsOnGravel() const { return GetGravelWheelCount() > 0; }
	int32 GetGravelWheelCount() const;

	const FVehicleParams& GetParams() const { return Params; }
	void SetParams(const FVehicleParams& InParams);

private:
	FVehicleParams Params;
	physx::PxRigidDynamic* ChassisActor = nullptr;

	FWheelSimulationState WheelStates[4];

	// 파워트레인 & 텔레메트리 상태
	int32 CurrentGear = 1;            // -1: R, 0: N, 1~8: 전진 기어
	float EngineRPM = 4200.0f;
	float ShiftCooldownTimer = 0.0f;
	float LateralG = 0.0f;
	float LongitudinalG = 0.0f;
	bool bPrevVelocityValid = false;
	FVector PrevWorldVelocity = FVector::ZeroVector;
	bool bTractionControlEnabled = true;
	bool bTCActive = false;
	bool bDRSRequested = false;
	bool bDRSActive = false;
	float GravelTime = 0.0f;

	// PIMPL로 PhysX Vehicle SDK 내부 헤더 의존성 격리
	struct FPhysXVehicleData* PhysXData = nullptr;

	void SetupPhysXData();
	void CleanupPhysXData();
	void UpdateDrivetrainAndShift(float DeltaTime, float CurrentForwardSpeed, float Throttle);
	float EvaluateEngineTorqueScale(float InRPM) const;
};

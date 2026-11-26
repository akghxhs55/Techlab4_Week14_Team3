#pragma once

#include "Components/PawnMovementComponent.h"
#include "Physics/VehicleParams.h"
#include "Physics/VehicleSimulation.h"
#include "VehicleMovementComponent.generated.h"

class UPrimitiveComponent;

/**
 * 4륜 자동차 물리 무브먼트 컴포넌트
 * PhysX Vehicle 2.0 및 FVehicleSimulationPipeline과 연동하여
 * 서스펜션, 타이어 마찰, 스로틀/조향/브레이크 물리 제어를 수행합니다.
 */
UCLASS()
class UVehicleMovementComponent : public UPawnMovementComponent
{
	GENERATED_BODY()

public:
	UVehicleMovementComponent();
	virtual ~UVehicleMovementComponent() override = default;

	virtual void InitializeComponent() override;
	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime) override;

	// ---- 주행 입력 인터페이스 (Unreal WheeledVehicle 호환) ----
	void SetThrottleInput(float InThrottle);
	void SetSteeringInput(float InSteering);
	void SetBrakeInput(float InBrake);
	void SetHandbrakeInput(bool bInHandbrake);

	float GetThrottleInput() const { return RawThrottleInput; }
	float GetSteeringInput() const { return RawSteeringInput; }
	float GetBrakeInput() const { return RawBrakeInput; }
	bool GetHandbrakeInput() const { return bRawHandbrakeInput; }

	// ---- 차량 물리 파라미터 ----
	FVehicleParams& GetVehicleParams() { return VehicleParams; }
	const FVehicleParams& GetVehicleParams() const { return VehicleParams; }
	void SetVehicleParams(const FVehicleParams& InParams);

	// ---- 주행 상태 조회 ----
	float GetForwardSpeed() const;      // 전진 속도 (m/s)
	float GetForwardSpeedKmH() const;   // 전진 속도 (km/h)
	bool IsInAir() const;               // 4륜 모두 체공 여부

	// 파워트레인 & 텔레메트리 조회
	int32 GetCurrentGear() const;       // 현재 기어 (-1: R, 1~8)
	float GetEngineRPM() const;
	float GetMaxRPM() const;
	float GetLateralG() const;
	float GetLongitudinalG() const;
	float GetAverageRearSlipRatio() const;
	bool IsTCActive() const;
	void SetDRSRequested(bool bEnable);
	bool IsDRSActive() const;
	bool IsDRSRequested() const { return bDRSRequested; }
	bool IsOnGravel() const;
	int32 GetGravelWheelCount() const;

	// 바퀴별 시뮬레이션 상태 및 트랜스폼 조회 (0: FL, 1: FR, 2: RL, 3: RR)
	const FWheelSimulationState& GetWheelState(EVehicleWheelOrder Order) const;
	const FWheelSimulationState& GetWheelState(int32 WheelIndex) const;
	FTransform GetWheelLocalTransform(int32 WheelIndex) const;
	FTransform GetWheelWorldTransform(int32 WheelIndex) const;

	// ---- 에디터 튜닝용 프로퍼티 ----
	UPROPERTY(EditAnywhere)
	float PeakTorque = 4000.0f;         // 엔진 최대 구동 토크 (N*m)

	UPROPERTY(EditAnywhere)
	float MaxSteerAngle = 35.0f;        // 전륜 최대 조향각 (도)

	UPROPERTY(EditAnywhere)
	float ChassisMass = 1500.0f;        // 차체 질량 (kg)

	UPROPERTY(EditAnywhere)
	float SuspensionStiffness = 35000.0f;// 서스펜션 스프링 강성 (N/m)

	UPROPERTY(EditAnywhere)
	float SuspensionDamping = 4500.0f;   // 서스펜션 댐퍼 감쇠 계수 (N*s/m)

	UPROPERTY(EditAnywhere)
	float WheelRadius = 0.35f;          // 바퀴 반경 (m)

	UPROPERTY(EditAnywhere)
	bool bReverseAsBrake = true;        // 전진 주행 중 후진 입력 시 제동 후 후진

	UPROPERTY(EditAnywhere)
	float ThrottleInputRate = 4.0f;     // 스로틀 반응 속도 (1/s)

	UPROPERTY(EditAnywhere)
	float SteeringInputRate = 6.0f;     // 조향 반응 속도 (1/s)

	UPROPERTY(EditAnywhere)
	float DownforceScale = 1.0f;        // 공력 다운포스 배율 (1.0 = 기본 프리셋)

	UPROPERTY(EditAnywhere)
	float TireGripScale = 1.0f;         // 타이어 마찰력 배율 (1.0 = 기본 프리셋)

	UPROPERTY(EditAnywhere)
	bool bEnableTractionControl = true; // F1 트랙션 컨트롤(TCS) 활성화 여부

protected:
	bool TryInitVehicle();

	FVehicleParams VehicleParams;
	FVehicleSimulationPipeline SimulationPipeline;
	bool bVehicleInitialized = false;

	// 원시 목표 입력
	float RawThrottleInput = 0.0f;
	float RawSteeringInput = 0.0f;
	float RawBrakeInput = 0.0f;
	bool bRawHandbrakeInput = false;

	// 스무딩된 현재 적용 입력
	float CurrentThrottle = 0.0f;
	float CurrentSteer = 0.0f;
	float CurrentBrake = 0.0f;
	bool bDRSRequested = false;
};

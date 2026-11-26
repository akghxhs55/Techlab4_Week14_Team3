#include "EnginePCH.h"
#include "VehicleParams.h"

#include "PhysXConversion.h"
#include "vehicle/PxVehicleAPI.h"

using namespace physx;

FVehicleParams::FVehicleParams()
{
	SetupFerrariF1();
}

void FVehicleParams::SetupDefaultCar()
{
	SetupFerrariF1();
}

void FVehicleParams::SetupFerrariF1()
{
	ChassisMass = 850.0f; // F1 차량 규정 최소 중량(~798kg) 기반 레이싱 세팅
	PeakTorque = 4500.0f; // V6 터보 하이브리드 파워유닛 (1000마력+)
	DownforceCoeff = 2.6f;            // 고속(288km/h)에서 차체 중량 ~2배 수준의 다운포스 생성
	AeroDragCoeff = 1.6f;             // 최고속도 약 315~320 km/h 균형
	DRSDragScale = 0.72f;             // DRS 개방 시 ~345 km/h까지 직선 추월 가속
	DRSDownforceScale = 0.88f;
	GroundEffectStrength = 0.25f;     // 플로어 차고 압축 시 최대 +25% 그라운드 이펙트 다운포스
	TireLateralFriction = 2.12f;      // 전륜 F1 슬릭 타이어 고그립 (100km/h+ 턴인 향상)
	RearTireLateralFriction = 2.48f;  // 후륜 광폭 슬릭 타이어 고그립 (스핀 방지 밸런스 유지)
	TireLongitudinalFriction = 1.90f; // 급가속·풀브레이킹 트랙션 한계
	TireNominalLoad = 2800.0f;
	TireLoadSensitivity = 0.12f;
	CorneringStiffness = 200000.0f;   // 횡강성 최댓값. 정지 시 약 87000, 200km/h에서 약 130000
	MinHighSpeedSteerScale = 0.13f;   // 초고속 직선/130R에서 4.2도 내외 정밀 조향
	HighSpeedSteerRefSpeed = 24.0f;   // 86km/h 기준 감쇠 (중속 코너링 ~14도 확보)
	FrontAntiRollStiffness = 36000.0f;
	RearAntiRollStiffness = 32000.0f;
	FinalDriveRatio = 3.80f;
	IdleRPM = 4200.0f;
	MaxRPM = 13500.0f;
	UpshiftRPM = 12600.0f;
	DownshiftRPM = 7800.0f;

	// 전륜 좌측 (Wheel 0) - 실측: X = +1.522m, Y = -0.800m, Z = -0.22m, 반경 0.358m, 폭 0.305m
	FrontLeftWheel.Offset = FVector(1.522f, -0.800f, -0.22f);
	FrontLeftWheel.Radius = 0.358f;
	FrontLeftWheel.HalfWidth = 0.152f;
	FrontLeftWheel.Mass = 15.0f;
	FrontLeftWheel.DampingRate = 0.25f;
	FrontLeftWheel.SuspensionTravelDist = 0.15f; // F1 고강성 짧은 서스펜션 스트로크
	FrontLeftWheel.SpringStiffness = 45000.0f;  // F1 고강성 레이싱 스프링
	FrontLeftWheel.SpringDamping = 6000.0f;
	FrontLeftWheel.SprungMass = ChassisMass * 0.25f;
	FrontLeftWheel.MaxSteerAngle = 32.0f;
	FrontLeftWheel.MaxBrakeTorque = 3000.0f;    // 카본 브레이크의 강력한 제동력
	FrontLeftWheel.MaxHandbrakeTorque = 0.0f;

	// 전륜 우측 (Wheel 1) - 실측: X = +1.522m, Y = +0.800m, Z = -0.22m
	FrontRightWheel = FrontLeftWheel;
	FrontRightWheel.Offset = FVector(1.522f, 0.800f, -0.22f);

	// 후륜 좌측 (Wheel 2) - 실측: X = -2.011m, Y = -0.762m, Z = -0.22m, 반경 0.358m, 광폭 0.405m
	RearLeftWheel = FrontLeftWheel;
	RearLeftWheel.Offset = FVector(-2.011f, -0.762f, -0.22f);
	RearLeftWheel.HalfWidth = 0.202f; // F1 후륜 광폭 타이어
	RearLeftWheel.Mass = 18.0f;
	RearLeftWheel.SpringStiffness = 48000.0f;
	RearLeftWheel.SpringDamping = 6500.0f;
	RearLeftWheel.MaxSteerAngle = 0.0f;
	RearLeftWheel.MaxBrakeTorque = 2500.0f;
	RearLeftWheel.MaxHandbrakeTorque = 3500.0f;

	// 후륜 우측 (Wheel 3) - 실측: X = -2.011m, Y = +0.762m, Z = -0.22m
	RearRightWheel = RearLeftWheel;
	RearRightWheel.Offset = FVector(-2.011f, 0.762f, -0.22f);
}

void FVehicleParams::SetupKoenigseggGemera()
{
	ChassisMass = 1850.0f;
	PeakTorque = 4500.0f; // 1700hp / 3500+ Nm
	DownforceCoeff = 0.65f;
	AeroDragCoeff = 0.95f;
	GroundEffectStrength = 0.10f;
	TireLateralFriction = 1.35f;
	RearTireLateralFriction = 1.55f;
	TireLongitudinalFriction = 1.35f;
	TireNominalLoad = 4500.0f;
	TireLoadSensitivity = 0.10f;
	CorneringStiffness = 93000.0f;    // 횡강성 최댓값. 정지 시 약 45000
	MinHighSpeedSteerScale = 0.15f;
	HighSpeedSteerRefSpeed = 20.0f;
	FrontAntiRollStiffness = 14000.0f;
	RearAntiRollStiffness = 12000.0f;
	FinalDriveRatio = 3.20f;
	IdleRPM = 1000.0f;
	MaxRPM = 8500.0f;
	UpshiftRPM = 7800.0f;
	DownshiftRPM = 4200.0f;

	// 전륜 좌측 (Wheel 0) - 실측: X = +1.483m, Y = -0.842m, Z = -0.20m, 21인치(반경 0.355m)
	FrontLeftWheel.Offset = FVector(1.483f, -0.842f, -0.2f);
	FrontLeftWheel.Radius = 0.355f;
	FrontLeftWheel.HalfWidth = 0.162f;
	FrontLeftWheel.Mass = 22.0f;
	FrontLeftWheel.DampingRate = 0.25f;
	FrontLeftWheel.SuspensionTravelDist = 0.25f;
	FrontLeftWheel.SpringStiffness = 38000.0f;
	FrontLeftWheel.SpringDamping = 4800.0f;
	FrontLeftWheel.SprungMass = ChassisMass * 0.25f;
	FrontLeftWheel.MaxSteerAngle = 35.0f;
	FrontLeftWheel.MaxBrakeTorque = 2000.0f;
	FrontLeftWheel.MaxHandbrakeTorque = 0.0f;

	// 전륜 우측 (Wheel 1) - 실측: X = +1.483m, Y = +0.842m, Z = -0.20m
	FrontRightWheel = FrontLeftWheel;
	FrontRightWheel.Offset = FVector(1.483f, 0.842f, -0.2f);

	// 후륜 좌측 (Wheel 2) - 실측: X = -1.515m, Y = -0.834m, Z = -0.20m, 22인치(반경 0.375m)
	RearLeftWheel = FrontLeftWheel;
	RearLeftWheel.Offset = FVector(-1.515f, -0.834f, -0.2f);
	RearLeftWheel.Radius = 0.375f;
	RearLeftWheel.HalfWidth = 0.174f;
	RearLeftWheel.MaxSteerAngle = 0.0f;
	RearLeftWheel.MaxHandbrakeTorque = 3000.0f;

	// 후륜 우측 (Wheel 3) - 실측: X = -1.515m, Y = +0.834m, Z = -0.20m
	RearRightWheel = RearLeftWheel;
	RearRightWheel.Offset = FVector(-1.515f, 0.834f, -0.2f);
}

FWheelConfig& FVehicleParams::GetWheel(EVehicleWheelOrder Order)
{
	switch (Order)
	{
	case EVehicleWheelOrder::FrontLeft:  return FrontLeftWheel;
	case EVehicleWheelOrder::FrontRight: return FrontRightWheel;
	case EVehicleWheelOrder::RearLeft:   return RearLeftWheel;
	case EVehicleWheelOrder::RearRight:  return RearRightWheel;
	default: return FrontLeftWheel;
	}
}

const FWheelConfig& FVehicleParams::GetWheel(EVehicleWheelOrder Order) const
{
	switch (Order)
	{
	case EVehicleWheelOrder::FrontLeft:  return FrontLeftWheel;
	case EVehicleWheelOrder::FrontRight: return FrontRightWheel;
	case EVehicleWheelOrder::RearLeft:   return RearLeftWheel;
	case EVehicleWheelOrder::RearRight:  return RearRightWheel;
	default: return FrontLeftWheel;
	}
}

FWheelConfig& FVehicleParams::GetWheel(int32 Index)
{
	return GetWheel(static_cast<EVehicleWheelOrder>(Index));
}

const FWheelConfig& FVehicleParams::GetWheel(int32 Index) const
{
	return GetWheel(static_cast<EVehicleWheelOrder>(Index));
}

PxVehicleFrame GetKurisuVehicleFrame()
{
	PxVehicleFrame Frame;
	Frame.lngAxis = PxVehicleAxes::ePosX; // +X = 전진 (Longitudinal)
	Frame.latAxis = PxVehicleAxes::ePosY; // +Y = 우측 (Lateral)
	Frame.vrtAxis = PxVehicleAxes::ePosZ; // +Z = 상향 (Vertical)
	return Frame;
}

PxVehicleScale GetKurisuVehicleScale()
{
	PxVehicleScale Scale;
	Scale.scale = 1.0f; // 1 unit = 1 meter
	return Scale;
}

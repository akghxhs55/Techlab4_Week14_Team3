#include "EnginePCH.h"
#include "VehicleMovementComponent.h"

#include "Components/PrimitiveComponent.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/Actor/WheeledVehiclePawn.h"
#include "Engine/World.h"
#include "Physics/PhysScene.h"
#include "Math/EngineMath.h"
#include "UObject/Casts.h"

#include <cmath>

UVehicleMovementComponent::UVehicleMovementComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	Velocity = FVector::ZeroVector;

	VehicleParams.SetupDefaultCar();
}

void UVehicleMovementComponent::InitializeComponent()
{
	Super::InitializeComponent();
}

void UVehicleMovementComponent::BeginPlay()
{
	Super::BeginPlay();
	TryInitVehicle();
}

bool UVehicleMovementComponent::TryInitVehicle()
{
	if (bVehicleInitialized)
	{
		return true;
	}

	UPrimitiveComponent* Prim = Cast<UPrimitiveComponent>(UpdatedComponent);
	if (!Prim && GetOwner())
	{
		Prim = Cast<UPrimitiveComponent>(GetOwner()->GetRootComponent());
		if (Prim && !UpdatedComponent)
		{
			SetUpdatedComponent(Prim);
		}
	}

	if (!Prim)
	{
		return false;
	}

	// 물리 시뮬레이션 보장
	if (!Prim->IsSimulatingPhysics())
	{
		Prim->SetSimulatePhysics(true);
	}

	if (Prim->BodyInstance.IsValidBodyInstance())
	{
		physx::PxRigidDynamic* RigidDynamic = Prim->BodyInstance.GetPxRigidDynamic();
		if (RigidDynamic)
		{
			// 소유 폰의 차체 메시에 따른 파라미터 자동 프리셋
			if (AWheeledVehiclePawn* Pawn = Cast<AWheeledVehiclePawn>(GetOwner()))
			{
				if (Pawn->GetMeshComponent() && Pawn->GetMeshComponent()->GetStaticMesh())
				{
					const FString MeshPath = Pawn->GetMeshComponent()->GetStaticMesh()->GetPath();
					if (MeshPath.find("Gemera") != FString::npos)
					{
						VehicleParams.SetupKoenigseggGemera();
					}
					else if (MeshPath.find("Ferrari") != FString::npos)
					{
						VehicleParams.SetupDefaultCar();
					}
				}
			}

			// 에디터 수정 프로퍼티 반영
			VehicleParams.PeakTorque = PeakTorque;
			VehicleParams.ChassisMass = ChassisMass;
			VehicleParams.FrontLeftWheel.MaxSteerAngle = MaxSteerAngle;
			VehicleParams.FrontRightWheel.MaxSteerAngle = MaxSteerAngle;
			VehicleParams.DownforceCoeff *= std::max(0.0f, DownforceScale);
			VehicleParams.TireLateralFriction *= std::max(0.1f, TireGripScale);
			VehicleParams.RearTireLateralFriction *= std::max(0.1f, TireGripScale);
			VehicleParams.TireLongitudinalFriction *= std::max(0.1f, TireGripScale);

			for (int32 i = 0; i < 4; ++i)
			{
				FWheelConfig& Wheel = VehicleParams.GetWheel(i);
				Wheel.SpringStiffness = SuspensionStiffness;
				Wheel.SpringDamping = SuspensionDamping;
				Wheel.Radius = WheelRadius;
			}

			SimulationPipeline.SetTractionControlEnabled(bEnableTractionControl);
			SimulationPipeline.Initialize(VehicleParams, RigidDynamic);
			bVehicleInitialized = true;
			return true;
		}
	}

	return false;
}

void UVehicleMovementComponent::TickComponent(float DeltaTime)
{
	Super::TickComponent(DeltaTime);

	if (!bVehicleInitialized)
	{
		if (!TryInitVehicle())
		{
			return;
		}
	}

	// 1. Pawn 입력 벡터 처리 (WASD 플레이어 컨트롤러 대응)
	const FVector InputVector = ConsumeInputVector();
	if (InputVector.SizeSquared() > 0.0001f && UpdatedComponent)
	{
		const FQuat WorldRot = UpdatedComponent->GetWorldRotation().Quaternion();
		const FVector ForwardDir = WorldRot.GetForwardVector();
		const FVector RightDir = WorldRot.GetRightVector();
		const float ForwardInput = InputVector.Dot(ForwardDir);
		const float RightInput = InputVector.Dot(RightDir);

		const float ForwardSpeed = GetForwardSpeed();
		if (ForwardInput > 0.0f)
		{
			RawThrottleInput = ForwardInput;
			RawBrakeInput = 0.0f;
		}
		else if (ForwardInput < 0.0f)
		{
			if (bReverseAsBrake && ForwardSpeed > 1.0f)
			{
				RawThrottleInput = 0.0f;
				RawBrakeInput = -ForwardInput;
			}
			else
			{
				RawThrottleInput = ForwardInput;
				RawBrakeInput = 0.0f;
			}
		}
		else
		{
			RawThrottleInput = 0.0f;
			RawBrakeInput = 0.0f;
		}

		RawSteeringInput = FMath::Clamp(RightInput, -1.0f, 1.0f);
	}
	else
	{
		// 키 입력을 떼면 즉시 조작 입력을 0으로 초기화
		RawThrottleInput = 0.0f;
		RawSteeringInput = 0.0f;
		RawBrakeInput = 0.0f;
	}

	// 2. 조작 입력 스무딩 보간 (고속일수록 키보드 조향 응답을 부드럽게 완화)
	const float HighSpeedSteerDamp = FMath::Lerp(1.0f, 0.55f, FMath::Clamp(std::abs(GetForwardSpeed()) / 60.0f, 0.0f, 1.0f));
	CurrentThrottle = FMath::FInterpTo(CurrentThrottle, RawThrottleInput, DeltaTime, ThrottleInputRate);
	CurrentSteer = FMath::FInterpTo(CurrentSteer, RawSteeringInput, DeltaTime, SteeringInputRate * HighSpeedSteerDamp);
	CurrentBrake = FMath::FInterpTo(CurrentBrake, RawBrakeInput, DeltaTime, 10.0f);

	// 3. PhysX 시뮬레이션 파이프라인 구동
	if (bDRSRequested && (CurrentBrake >= 0.05f || GetForwardSpeed() < 15.0f))
	{
		bDRSRequested = false;
	}
	SimulationPipeline.SetTractionControlEnabled(bEnableTractionControl);
	SimulationPipeline.SetDRSRequested(bDRSRequested);
	UWorld* World = GetWorld();
	if (World && World->GetPhysicsScene())
	{
		physx::PxScene* PxScene = World->GetPhysicsScene()->GetPxScene();
		if (PxScene)
		{
			SimulationPipeline.Update(DeltaTime, PxScene, CurrentThrottle, CurrentSteer, CurrentBrake, bRawHandbrakeInput);
		}
	}

	// 4. 컴포넌트 Velocity 동기화
	if (UpdatedComponent)
	{
		Velocity = UpdatedComponent->GetWorldRotation().Quaternion().GetForwardVector() * GetForwardSpeed();
	}
}

void UVehicleMovementComponent::SetThrottleInput(float InThrottle)
{
	RawThrottleInput = FMath::Clamp(InThrottle, -1.0f, 1.0f);
}

void UVehicleMovementComponent::SetSteeringInput(float InSteering)
{
	RawSteeringInput = FMath::Clamp(InSteering, -1.0f, 1.0f);
}

void UVehicleMovementComponent::SetBrakeInput(float InBrake)
{
	RawBrakeInput = FMath::Clamp(InBrake, 0.0f, 1.0f);
}

void UVehicleMovementComponent::SetHandbrakeInput(bool bInHandbrake)
{
	bRawHandbrakeInput = bInHandbrake;
}

void UVehicleMovementComponent::SetVehicleParams(const FVehicleParams& InParams)
{
	VehicleParams = InParams;
	if (bVehicleInitialized)
	{
		SimulationPipeline.SetParams(VehicleParams);
	}
}

float UVehicleMovementComponent::GetForwardSpeed() const
{
	if (!bVehicleInitialized)
	{
		return 0.0f;
	}
	return SimulationPipeline.GetForwardSpeed();
}

float UVehicleMovementComponent::GetForwardSpeedKmH() const
{
	return GetForwardSpeed() * 3.6f;
}

bool UVehicleMovementComponent::IsInAir() const
{
	if (!bVehicleInitialized)
	{
		return true;
	}

	for (int32 i = 0; i < 4; ++i)
	{
		if (SimulationPipeline.GetWheelState(i).bInContact)
		{
			return false;
		}
	}
	return true;
}

int32 UVehicleMovementComponent::GetCurrentGear() const
{
	return bVehicleInitialized ? SimulationPipeline.GetCurrentGear() : 1;
}

float UVehicleMovementComponent::GetEngineRPM() const
{
	return bVehicleInitialized ? SimulationPipeline.GetEngineRPM() : VehicleParams.IdleRPM;
}

float UVehicleMovementComponent::GetMaxRPM() const
{
	return VehicleParams.MaxRPM;
}

float UVehicleMovementComponent::GetLateralG() const
{
	return bVehicleInitialized ? SimulationPipeline.GetLateralG() : 0.0f;
}

float UVehicleMovementComponent::GetLongitudinalG() const
{
	return bVehicleInitialized ? SimulationPipeline.GetLongitudinalG() : 0.0f;
}

float UVehicleMovementComponent::GetAverageRearSlipRatio() const
{
	return bVehicleInitialized ? SimulationPipeline.GetAverageRearSlipRatio() : 0.0f;
}

bool UVehicleMovementComponent::IsTCActive() const
{
	return bVehicleInitialized ? SimulationPipeline.IsTCActive() : false;
}

void UVehicleMovementComponent::SetDRSRequested(bool bEnable)
{
	bDRSRequested = bEnable;
	if (bVehicleInitialized)
	{
		SimulationPipeline.SetDRSRequested(bEnable);
	}
}

bool UVehicleMovementComponent::IsDRSActive() const
{
	return bVehicleInitialized ? SimulationPipeline.IsDRSActive() : false;
}

bool UVehicleMovementComponent::IsOnGravel() const
{
	return bVehicleInitialized ? SimulationPipeline.IsOnGravel() : false;
}

int32 UVehicleMovementComponent::GetGravelWheelCount() const
{
	return bVehicleInitialized ? SimulationPipeline.GetGravelWheelCount() : 0;
}

const FWheelSimulationState& UVehicleMovementComponent::GetWheelState(EVehicleWheelOrder Order) const
{
	return SimulationPipeline.GetWheelState(Order);
}

const FWheelSimulationState& UVehicleMovementComponent::GetWheelState(int32 WheelIndex) const
{
	return SimulationPipeline.GetWheelState(WheelIndex);
}

FTransform UVehicleMovementComponent::GetWheelLocalTransform(int32 WheelIndex) const
{
	if (!bVehicleInitialized)
	{
		return FTransform::Identity;
	}
	return SimulationPipeline.GetWheelState(WheelIndex).LocalPose;
}

FTransform UVehicleMovementComponent::GetWheelWorldTransform(int32 WheelIndex) const
{
	const FTransform LocalPose = GetWheelLocalTransform(WheelIndex);
	if (UpdatedComponent)
	{
		const FVector BodyLoc = UpdatedComponent->GetWorldLocation();
		const FQuat BodyRot = UpdatedComponent->GetWorldRotation().Quaternion();

		FTransform WorldPose;
		WorldPose.Location = BodyLoc + BodyRot.RotateVector(LocalPose.Location);
		WorldPose.Rotation = BodyRot * LocalPose.Rotation;
		WorldPose.Scale = LocalPose.Scale;
		return WorldPose;
	}
	return LocalPose;
}

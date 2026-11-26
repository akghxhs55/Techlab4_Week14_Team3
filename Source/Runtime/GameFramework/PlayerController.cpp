#include "EnginePCH.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/Character.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/SwordCharacter.h"
#include "GameFramework/TestCharacter.h"
#include "GameFramework/GameMode.h"
#include "GameFramework/GameHUD.h"
#include "Engine/Engine.h"
#include "Camera/PlayerCameraManager.h" 
#include "Engine/World.h"               
#include "Camera/CameraShakeBase.h"
#include <cstring>
#include "GameFramework/DefaultPawn.h"
#include "GameFramework/Actor/WheeledVehiclePawn.h"

APlayerController::APlayerController()
	: ControlRotation(FRotator::Identitiy)
{
	PrimaryActorTick.bCanEverTick = true;
}

void APlayerController::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	ProcessPlayerInput(DeltaTime);
}

void APlayerController::ProcessPlayerInput(float DeltaTime)
{
	if (!PossessedPawn) return;
	if (GetWorld() && GetWorld()->GetGameMode() && !GetWorld()->GetGameMode()->AllowsPlayerControllerInput())
	{
		MouseAxisX = MouseAxisY = 0.0f;
		return;
	}
	const float Forward = static_cast<float>(bKeyDown[static_cast<int32>(EKeyCode::W)]) -
		static_cast<float>(bKeyDown[static_cast<int32>(EKeyCode::S)]);
	const float Right = static_cast<float>(bKeyDown[static_cast<int32>(EKeyCode::D)]) -
		static_cast<float>(bKeyDown[static_cast<int32>(EKeyCode::A)]);
	
	PossessedPawn->AddControllerYawInput(MouseAxisX * LookSensitivity);
	PossessedPawn->AddControllerPitchInput(MouseAxisY * LookSensitivity);

	// ControlRotation 업데이트
	ControlRotation.Yaw += MouseAxisX * LookSensitivity;
	ControlRotation.Pitch = FMath::Clamp(
		ControlRotation.Pitch + MouseAxisY * LookSensitivity,
		-89.9f, 89.9f);

	MouseAxisX = MouseAxisY = 0.0f;
	const FQuat Rotation = PossessedPawn->GetMovementInputRotation().Quaternion();
	// UE와 같이 축 값(-1~1)만 넘긴다. DeltaTime은 입력을 소비해 실제로 움직이는 쪽(MovementComponent / Pawn Tick)이 곱한다.
	if (Forward != 0.0f) PossessedPawn->AddMovementInput(Rotation.GetForwardVector(), Forward);
	if (Right != 0.0f) PossessedPawn->AddMovementInput(Rotation.GetRightVector(), Right);
	if (Cast<ADefaultPawn>(PossessedPawn))
	{
		const float Up = static_cast<float>(bKeyDown[static_cast<int32>(EKeyCode::E)]) -
			static_cast<float>(bKeyDown[static_cast<int32>(EKeyCode::Q)]);
		if (Up != 0.0f) PossessedPawn->AddMovementInput(FVector(0, 0, 1), Up);
	}
}

void APlayerController::Possess(APawn* InPawn)
{
	if (PossessedPawn == InPawn) return;
	UnPossess();
	PossessedPawn = InPawn;
	if (PossessedPawn)
	{
		PossessedPawn->PossessedBy(this);
		ControlRotation = PossessedPawn->GetActorRotation();
	}
	if (PlayerCameraManager) PlayerCameraManager->SetViewTarget(PossessedPawn);
}

void APlayerController::UnPossess()
{
	if (ATestCharacter* Character = Cast<ATestCharacter>(PossessedPawn))
		Character->StopAim();
	if (PossessedPawn) PossessedPawn->UnPossessed();
	PossessedPawn = nullptr;
}

void APlayerController::InputKey(int32 Key, bool bDown)
{
	if (Key < 0 || Key >= 256) return;
	const bool bJustPressed = bDown && !bKeyDown[Key];
	bKeyDown[Key] = bDown;
	ACharacter* Character = Cast<ACharacter>(PossessedPawn);
	const bool bInputAllowed = GetWorld() && !GetWorld()->IsPaused() &&
		(!GetWorld()->GetGameMode() || GetWorld()->GetGameMode()->AllowsPlayerControllerInput());
	if (bJustPressed && (Key == static_cast<int32>(EKeyCode::K) || Key == static_cast<int32>(EKeyCode::L)))
		KRS_LOG(Info, "[RagdollInput] key={}, allowed={}, pawn={}, testCharacter={}", Key,
			bInputAllowed, PossessedPawn ? PossessedPawn->GetName() : FString("none"),
			Cast<ATestCharacter>(PossessedPawn) != nullptr);
	if (Key == static_cast<int32>(EKeyCode::RButton))
	{
		if (ATestCharacter* TestCharacter = Cast<ATestCharacter>(PossessedPawn))
		{
			if (!bDown) TestCharacter->StopAim();
			else if (bJustPressed && bInputAllowed)
				TestCharacter->StartAim();
		}
	}
	if (Key == static_cast<int32>(EKeyCode::Space) && Character)
	{
		if (!bDown) Character->StopJumping();
		else if (bJustPressed && bInputAllowed &&
			(Cast<ASwordCharacter>(PossessedPawn) || Cast<ATestCharacter>(PossessedPawn)))
			Character->Jump();
	}
	if ((Key == static_cast<int32>(EKeyCode::F) || Key == static_cast<int32>(EKeyCode::LButton)) &&
		bJustPressed && Character && bInputAllowed)
		Character->Attack();
	if ((Key == static_cast<int32>(EKeyCode::Shift) ||
		Key == static_cast<int32>(EKeyCode::LShift) ||
		Key == static_cast<int32>(EKeyCode::RShift)) && bJustPressed && bInputAllowed)
	{
		if (ATestCharacter* TestCharacter = Cast<ATestCharacter>(PossessedPawn))
			TestCharacter->Dash();
	}
	if (Key == static_cast<int32>(EKeyCode::R) && bJustPressed && bInputAllowed)
	{
		if (ASwordCharacter* SwordCharacter = Cast<ASwordCharacter>(PossessedPawn))
			SwordCharacter->CastBeam();
	}
	if (AWheeledVehiclePawn* Vehicle = Cast<AWheeledVehiclePawn>(PossessedPawn))
	{
		if (Key == static_cast<int32>(EKeyCode::Space))
		{
			Vehicle->SetHandbrakeInput(bDown);
		}
		else if (Key == static_cast<int32>(EKeyCode::E) && bJustPressed && bInputAllowed)
		{
			Vehicle->ToggleDRS();
		}
		else if (Key == static_cast<int32>(EKeyCode::Q) && bJustPressed && bInputAllowed)
		{
			Vehicle->ToggleTractionControl();
		}
	}
	// Physics Asset이 있는 캐릭터의 바디를 물리 시뮬레이션으로 전환한다.
	if (Key == static_cast<int32>(EKeyCode::F9) && bJustPressed && bInputAllowed && Character)
	{
		if (Character->GetSkeletalMeshComponent()->IsRagdollActive()) Character->StopRagdoll();
		else Character->StartRagdoll();
	}
	if (bJustPressed && bInputAllowed)
	{
		if (ATestCharacter* TestCharacter = Cast<ATestCharacter>(PossessedPawn))
		{
			if (Key == static_cast<int32>(EKeyCode::K))
			{
				const bool bStarted = TestCharacter->StartRagdoll();
				KRS_LOG(Info, "[RagdollInput] K start result={}, assetPresent={}", bStarted,
					TestCharacter->GetSkeletalMeshComponent()->GetPhysicsAsset() != nullptr);
			}
			else if (Key == static_cast<int32>(EKeyCode::L))
			{
				TestCharacter->StopRagdoll();
				KRS_LOG(Info, "[RagdollInput] L stop, active={}",
					TestCharacter->GetSkeletalMeshComponent()->IsRagdollActive());
			}
		}
	}
	if (!bInputAllowed)
		return;
	if (!bJustPressed || !PlayerCameraManager) return;

	if (Key == static_cast<int32>(EKeyCode::F6))
	{
		UClass* ShakeClass = UCameraShakeBase::StaticClass();
		auto* Settings = static_cast<UCameraShakeBase*>(
			ShakeClass->GetDefaultObject());

		if (!Settings) return;

		// 새 Shake를 생성할 때 복사할 테스트용 기본 설정.
		Settings->Duration = 1.0f;
		Settings->BlendInTime = 0.05f;
		Settings->BlendOutTime = 0.3f;

		Settings->RotationPitch.Amplitude = 2.0f;
		Settings->RotationPitch.Frequency = 10.0f;

		Settings->RotationYaw.Amplitude = 1.0f;
		Settings->RotationYaw.Frequency = 13.0f;

		// 이전 테스트를 끊고 새로 실행한다.
		PlayerCameraManager->StopAllCameraShakes(true);
		PlayerCameraManager->StartCameraShake(ShakeClass, 1.0f);

	}
	else if (Key == static_cast<int32>(EKeyCode::F7))
	{
		PlayerCameraManager->StopAllCameraShakes(true);
	}
}

void APlayerController::InputAxis(EGameInputAxis Axis, float Delta)
{
	if (Axis == EGameInputAxis::MouseX) MouseAxisX += Delta;
	else if (Axis == EGameInputAxis::MouseY) MouseAxisY += Delta;
}

void APlayerController::ClearInputState()
{
	if (PossessedPawn)
	{
		if (ATestCharacter* Character = Cast<ATestCharacter>(PossessedPawn)) Character->StopAim();
		if (ACharacter* Character = Cast<ACharacter>(PossessedPawn)) Character->StopJumping();
		PossessedPawn->ConsumeMovementInputVector();
	}
	std::memset(bKeyDown, 0, sizeof(bKeyDown));
	MouseAxisX = MouseAxisY = 0.0f;
}
void APlayerController::SpawnPlayerCameraManager()
{
	// 중복생성방지
	if (PlayerCameraManager || !GetWorld()) return;

	PlayerCameraManager = GetWorld()->SpawnActor<APlayerCameraManager>();
	PlayerCameraManager->InitializeFor(this);
}
void APlayerController::UpdateCameraManager(float DeltaTime)
{
	if (PlayerCameraManager)
	{
		PlayerCameraManager->UpdateCamera(DeltaTime);
	}
}

void APlayerController::SetCursorMode(ECursorMode Mode)
{
	CursorMode = Mode;
	MouseAxisX = MouseAxisY = 0.0f;
	if (GEngine)
		GEngine->SetGameMouseCapture(GetWorld(), Mode == ECursorMode::Locked);
}

void APlayerController::EndPlay()
{
	if (CursorMode == ECursorMode::Locked)
		SetCursorMode(ECursorMode::Free);
	Super::EndPlay();
}

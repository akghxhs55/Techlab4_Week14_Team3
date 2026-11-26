#include "EnginePCH.h"
#include "GameFramework/Actor/WheeledVehiclePawn.h"
#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/VehicleMovementComponent.h"
#include "Components/AudioComponent.h"
#include "Components/ParticleSystemComponent.h"
#include "GameFramework/SpringArmComponent.h"
#include "Camera/CameraComponent.h"
#include "Asset/AssetManager.h"
#include "Audio/SoundWave.h"
#include "Particles/ParticleSystem.h"
#include "Particles/ParticleEmitter.h"
#include "Particles/ParticleLODLevel.h"
#include "Particles/Module/ParticleModuleRequired.h"
#include "Particles/Module/ParticleModuleSpawn.h"
#include "Particles/Module/ParticleModuleLifetime.h"
#include "Particles/Module/ParticleModuleSize.h"
#include "Particles/Module/ParticleModuleVelocity.h"
#include "Particles/Module/ParticleModuleColor.h"
#include "Particles/Module/ParticleModuleSubUV.h"
#include "Engine/World.h"
#include "UI/UserWidget.h"
#include "UI/PanelWidget.h"
#include "UI/TextBlock.h"
#include "UI/ProgressBar.h"
#include "UI/Image.h"
#include "Math/EngineMath.h"
#include "Core/Misc/Paths.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>

namespace
{
	// "1:32.456" 형식. 기록이 없으면 자리표시
	void FormatLapTime(float Seconds, char* Out, size_t OutSize)
	{
		if (Seconds <= 0.0f)
		{
			std::snprintf(Out, OutSize, "-:--.---");
			return;
		}
		const int32 Millis = static_cast<int32>(Seconds * 1000.0f + 0.5f);
		std::snprintf(Out, OutSize, "%d:%02d.%03d", Millis / 60000, (Millis / 1000) % 60, Millis % 1000);
	}

	std::filesystem::path GetLapRecordPath()
	{
		return (FPaths::HasProject() ? FPaths::ProjectDir() : FPaths::EngineDir()) / "Saved" / "LapTimes.csv";
	}

	// 같은 결승선(5 m 이내)에서 남긴 기록 중 가장 빠른 것. 없으면 0
	float LoadBestLapTime(const FVector& LineOrigin)
	{
		std::ifstream File(GetLapRecordPath());
		float Best = 0.0f;
		std::string Line;
		while (std::getline(File, Line))
		{
			const size_t Comma = Line.find(','); // 날짜 뒤부터 숫자
			if (Comma == std::string::npos) continue;
			float X = 0.0f, Y = 0.0f, Seconds = 0.0f;
			int Lap = 0;
			if (sscanf_s(Line.c_str() + Comma + 1, "%f,%f,%d,%f", &X, &Y, &Lap, &Seconds) != 4) continue;
			if (std::abs(X - LineOrigin.X) > 5.0f || std::abs(Y - LineOrigin.Y) > 5.0f) continue;
			if (Seconds > 0.0f && (Best <= 0.0f || Seconds < Best)) Best = Seconds;
		}
		return Best;
	}

	void AppendLapRecord(const FVector& LineOrigin, int32 Lap, float Seconds)
	{
		const std::filesystem::path Path = GetLapRecordPath();
		std::error_code Error;
		std::filesystem::create_directories(Path.parent_path(), Error);
		const bool bNewFile = !std::filesystem::exists(Path, Error);

		std::ofstream File(Path, std::ios::app);
		if (!File)
		{
			KRS_LOG(Warning, "[Lap] cannot write lap record file");
			return;
		}
		if (bNewFile)
		{
			File << "date,line_x,line_y,lap,seconds,time\n";
		}

		const std::time_t Now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
		std::tm Local{};
		localtime_s(&Local, &Now);
		char Date[32];
		std::strftime(Date, sizeof(Date), "%Y-%m-%d %H:%M:%S", &Local);

		char Time[32];
		FormatLapTime(Seconds, Time, sizeof(Time));
		File << Date << ',' << LineOrigin.X << ',' << LineOrigin.Y << ',' << Lap << ',' << Seconds << ',' << Time << '\n';
	}

	USoundWave* GetOrCreateF1EngineWave()
	{
		static USoundWave* CachedWave = nullptr;
		if (CachedWave) return CachedWave;

		CachedWave = NewObject<USoundWave>(nullptr);
		constexpr uint32 SampleRate = 44100;
		constexpr uint32 NumFrames = 44100;
		CachedWave->SampleRate = SampleRate;
		CachedWave->NumChannels = 1;
		CachedWave->NumFrames = NumFrames;
		CachedWave->Duration = 1.0f;
		CachedWave->PCMData.SetNum(NumFrames);

		constexpr double TwoPi = 6.283185307179586;
		// 110 Hz 기본음 + F1 V6 하이브리드 배기 하모닉 (정수 배수 주파수로 루프 이음새 제거)
		for (uint32 i = 0; i < NumFrames; ++i)
		{
			const double t = static_cast<double>(i) / static_cast<double>(SampleRate);
			double s = 0.0;
			s += 0.42 * std::sin(TwoPi * 110.0 * t);
			s += 0.30 * std::sin(TwoPi * 220.0 * t);
			s += 0.22 * std::sin(TwoPi * 330.0 * t);
			s += 0.14 * std::sin(TwoPi * 440.0 * t);
			s += 0.10 * std::sin(TwoPi * 660.0 * t);
			s += 0.06 * std::sin(TwoPi * 880.0 * t);
			// 소프트 클리핑으로 자연스러운 배기 질감 부여
			CachedWave->PCMData[i] = static_cast<float>(std::tanh(s * 1.15) * 0.45);
		}
		return CachedWave;
	}

	USoundWave* GetOrCreateAeroWindWave()
	{
		static USoundWave* CachedWave = nullptr;
		if (CachedWave) return CachedWave;

		CachedWave = NewObject<USoundWave>(nullptr);
		constexpr uint32 SampleRate = 44100;
		constexpr uint32 NumFrames = 44100;
		CachedWave->SampleRate = SampleRate;
		CachedWave->NumChannels = 1;
		CachedWave->NumFrames = NumFrames;
		CachedWave->Duration = 1.0f;
		CachedWave->PCMData.SetNum(NumFrames);

		uint32 Seed = 0x13579BDFu;
		float Filtered = 0.0f;
		for (uint32 i = 0; i < NumFrames; ++i)
		{
			Seed = Seed * 1664525u + 1013904223u;
			const float White = (static_cast<float>(Seed >> 8) / 8388608.0f) - 1.0f;
			Filtered = Filtered * 0.88f + White * 0.12f;
			// 루프 끝 페이드 크로스 블렌딩
			float Env = 1.0f;
			if (i < 256) Env = static_cast<float>(i) / 256.0f;
			else if (i > NumFrames - 256) Env = static_cast<float>(NumFrames - i) / 256.0f;
			CachedWave->PCMData[i] = Filtered * 0.55f * Env;
		}
		return CachedWave;
	}

	USoundWave* GetOrCreateTireSkidWave()
	{
		static USoundWave* CachedWave = nullptr;
		if (CachedWave) return CachedWave;

		CachedWave = NewObject<USoundWave>(nullptr);
		constexpr uint32 SampleRate = 44100;
		constexpr uint32 NumFrames = 44100;
		CachedWave->SampleRate = SampleRate;
		CachedWave->NumChannels = 1;
		CachedWave->NumFrames = NumFrames;
		CachedWave->Duration = 1.0f;
		CachedWave->PCMData.SetNum(NumFrames);

		constexpr double TwoPi = 6.283185307179586;
		// 타이어 "끼이익". 잡음을 섞으면 브레이크 바람 소리처럼 들리므로 음정이 있는 성분만 쓴다.
		// 주파수가 모두 정수라 1초 루프의 이음새가 없다
		for (uint32 i = 0; i < NumFrames; ++i)
		{
			const double t = static_cast<double>(i) / static_cast<double>(SampleRate);
			// 음정을 살짝 떨어 고무가 붙었다 미끄러지는 느낌을 낸다
			const double Vibrato = 2.5 * std::sin(TwoPi * 7.0 * t) + 0.8 * std::sin(TwoPi * 31.0 * t);
			const double Phase = TwoPi * 920.0 * t + Vibrato;
			const double Squeal = 0.60 * std::sin(Phase)
				+ 0.25 * std::sin(2.0 * Phase)
				+ 0.12 * std::sin(3.0 * Phase)
				+ 0.20 * std::sin(TwoPi * 967.0 * t + Vibrato);
			const double Flutter = 0.85 + 0.15 * std::sin(TwoPi * 23.0 * t);
			CachedWave->PCMData[i] = static_cast<float>(Squeal * Flutter * 0.22);
		}
		return CachedWave;
	}

	UParticleSystem* GetOrCreateTireSmokeTemplate()
	{
		static UParticleSystem* CachedSys = nullptr;
		if (CachedSys) return CachedSys;

		CachedSys = NewObject<UParticleSystem>(nullptr);
		UParticleEmitter* Emitter = NewObject<UParticleEmitter>(CachedSys);
		UParticleLODLevel* LODLevel = NewObject<UParticleLODLevel>(Emitter);
		LODLevel->SetLevel(0);

		// CloudAtlas는 8x8 아틀라스라 SubUV로 나눠야 한다. 파티클마다 칸 하나를 생성 때 한 번 뽑는다
		auto* Required = NewObject<UParticleModuleRequired>(LODLevel);
		Required->SetMaterial(UAssetManager::GetAssetByPath<UMaterial>("ParticleSmokeCloud"));
		Required->SetSubUV(EParticleSubUVInterpMethod::Random, 8, 8, 1.0f);
		LODLevel->SetRequiredModule(Required);

		auto* SubUV = NewObject<UParticleModuleSubUV>(LODLevel);
		SubUV->BakeDistributions();
		LODLevel->AddModule(SubUV);

		auto* Spawn = NewObject<UParticleModuleSpawn>(LODLevel);
		Spawn->GetRate().Mode = EDistributionMode::Constant;
		Spawn->GetRate().Constant = 36.0f;
		Spawn->GetRateScale().Mode = EDistributionMode::Constant;
		Spawn->GetRateScale().Constant = 1.0f;
		Spawn->BakeDistributions();
		LODLevel->SetSpawnModule(Spawn);

		auto* Lifetime = NewObject<UParticleModuleLifetime>(LODLevel);
		Lifetime->GetLifetime().Mode = EDistributionMode::Uniform;
		Lifetime->GetLifetime().MinValue = 0.35f;
		Lifetime->GetLifetime().MaxValue = 0.65f;
		Lifetime->BakeDistributions();
		LODLevel->AddModule(Lifetime);

		auto* Size = NewObject<UParticleModuleSize>(LODLevel);
		Size->GetStartSize().Mode = EDistributionMode::Uniform;
		Size->GetStartSize().MinValue = FVector(0.38f, 0.38f, 0.38f);
		Size->GetStartSize().MaxValue = FVector(0.75f, 0.75f, 0.75f);
		Size->BakeDistributions();
		LODLevel->AddModule(Size);

		auto* Velocity = NewObject<UParticleModuleVelocity>(LODLevel);
		Velocity->GetStartVelocity().Mode = EDistributionMode::Uniform;
		Velocity->GetStartVelocity().MinValue = FVector(-1.5f, -0.8f, 0.6f);
		Velocity->GetStartVelocity().MaxValue = FVector(0.5f, 0.8f, 1.8f);
		Velocity->GetStartVelocityRadial().Mode = EDistributionMode::Constant;
		Velocity->GetStartVelocityRadial().Constant = 0.4f;
		Velocity->BakeDistributions();
		LODLevel->AddModule(Velocity);

		auto* Color = NewObject<UParticleModuleColor>(LODLevel);
		Color->GetStartColor().Mode = EDistributionMode::Constant;
		// 구름 텍스처가 중간 회색이라 1보다 크게 곱해 밝은 타이어 연기로 만든다
		Color->GetStartColor().Constant = FVector(1.8f, 1.82f, 1.86f);
		Color->GetStartAlpha().Mode = EDistributionMode::Constant;
		Color->GetStartAlpha().Constant = 0.45f;
		Color->BakeDistributions();
		LODLevel->AddModule(Color);

		LODLevel->UpdateModuleLists();
		Emitter->AddLODLevel(LODLevel);
		Emitter->Build();
		CachedSys->AddEmitter(Emitter);
		return CachedSys;
	}
}

AWheeledVehiclePawn::AWheeledVehiclePawn()
{
	PrimaryActorTick.bCanEverTick = true;

	// 1. 차체 루트 충돌체 (스케일 1.0 유지 -> 자식 컴포넌트의 Shear 왜곡 방지)
	ChassisCollision = CreateDefaultSubobject<UBoxComponent>("ChassisCollision");
	SetRootComponent(ChassisCollision);
	ChassisCollision->BoxExtent = FVector(2.76f, 0.96f, 0.44f); // 롤 관성(폭)은 유지하고 하부 높이만 올려 연석 걸림 방지
	ChassisCollision->BodyInstance.bSimulatePhysics = true;
	ChassisCollision->BodyInstance.bEnableGravity = true;
	ChassisCollision->BodyInstance.CollisionEnabled = ECollisionEnabled::QueryAndPhysics;
	ChassisCollision->BodyInstance.MassInKgOverride = 850.0f; // F1 레이싱 카 총중량
	ChassisCollision->BodyInstance.LinearDamping = 0.01f;
	ChassisCollision->BodyInstance.AngularDamping = 0.32f;

	// 2. 차체 렌더링용 비주얼 메시 (ChassisCollision의 자식으로 붙이며, 자식을 갖지 않음)
	MeshComponent = CreateDefaultSubobject<UStaticMeshComponent>("Mesh");
	MeshComponent->SetupAttachment(ChassisCollision);
	MeshComponent->BodyInstance.bSimulatePhysics = false;
	MeshComponent->BodyInstance.CollisionEnabled = ECollisionEnabled::NoCollision;

	// 차체 메시 검색: Gemera 우선 -> Ferrari F1 -> Ferrari -> Cube 대체
	UStaticMesh* ChassisMesh = UAssetManager::GetAssetByPath<UStaticMesh>("Meshes/Gemera/Gemera_Chassis.obj");
	if (!ChassisMesh)
	{
		ChassisMesh = UAssetManager::GetAssetByPath<UStaticMesh>("Meshes/FerrariF1/FerrariF1_Chassis.obj");
	}
	if (!ChassisMesh)
	{
		ChassisMesh = UAssetManager::GetAssetByPath<UStaticMesh>("Meshes/Ferrari/Ferrari_Chassis.obj");
	}
	const bool bIsCustomChassis = (ChassisMesh != nullptr);
	if (!ChassisMesh)
	{
		ChassisMesh = UAssetManager::GetAssetByPath<UStaticMesh>("Cube");
	}
	if (ChassisMesh)
	{
		MeshComponent->SetStaticMesh(ChassisMesh);
	}

	if (bIsCustomChassis)
	{
		// 물리 평형 시 바퀴 중심(Chassis 기준 -0.324m)과 F1 차체 허브(+0.334m)를 일치시켜 바퀴와 서스펜션 암 정렬
		MeshComponent->SetRelativeLocation(FVector(0.0f, 0.0f, -0.658f));
		MeshComponent->SetRelativeScale3D(FVector(1.0f, 1.0f, 1.0f));
	}
	else
	{
		MeshComponent->SetRelativeLocation(FVector(0.0f, 0.0f, 0.0f));
		MeshComponent->SetRelativeScale3D(FVector(4.0f, 2.0f, 1.0f));
	}

	// 3. 차량 무브먼트 컴포넌트 (루트 물리 컴포넌트 바인딩)
	VehicleMovement = CreateDefaultSubobject<UVehicleMovementComponent>("VehicleMovement");
	VehicleMovement->SetUpdatedComponent(ChassisCollision);

	// 4. 스프링암 및 카메라 (차체에 맞춘 3인칭 레이싱 시점)
	SpringArmComponent = CreateDefaultSubobject<USpringArmComponent>("SpringArm");
	SpringArmComponent->SetupAttachment(ChassisCollision);
	SpringArmComponent->TargetArmLength = 7.5f;
	SpringArmComponent->TargetOffset = FVector(0.0f, 0.0f, 1.6f);
	SpringArmComponent->SetRelativeRotation(FRotator(15.0f, 0.0f, 0.0f));
	SpringArmComponent->bInheritPitch = false;
	SpringArmComponent->bInheritRoll = false;

	CameraComponent = CreateDefaultSubobject<UCameraComponent>("Camera");
	CameraComponent->SetupAttachment(SpringArmComponent);

	// 5. 바퀴 비주얼 컴포넌트 생성 (ChassisCollision 직속 자식 -> 부모 스케일 1.0으로 왜곡 없음)
	UStaticMesh* WheelMesh_FL_Asset = UAssetManager::GetAssetByPath<UStaticMesh>("Meshes/FerrariF1/FerrariF1_Wheel_FL.obj");
	UStaticMesh* WheelMesh_FR_Asset = UAssetManager::GetAssetByPath<UStaticMesh>("Meshes/FerrariF1/FerrariF1_Wheel_FR.obj");
	UStaticMesh* WheelMesh_RL_Asset = UAssetManager::GetAssetByPath<UStaticMesh>("Meshes/FerrariF1/FerrariF1_Wheel_RL.obj");
	UStaticMesh* WheelMesh_RR_Asset = UAssetManager::GetAssetByPath<UStaticMesh>("Meshes/FerrariF1/FerrariF1_Wheel_RR.obj");

	if (!WheelMesh_FL_Asset || !WheelMesh_FR_Asset || !WheelMesh_RL_Asset || !WheelMesh_RR_Asset)
	{
		UStaticMesh* F1Front = UAssetManager::GetAssetByPath<UStaticMesh>("Meshes/FerrariF1/FerrariF1_Wheel_Front.obj");
		UStaticMesh* F1Rear = UAssetManager::GetAssetByPath<UStaticMesh>("Meshes/FerrariF1/FerrariF1_Wheel_Rear.obj");
		if (F1Front && F1Rear)
		{
			if (!WheelMesh_FL_Asset) WheelMesh_FL_Asset = F1Front;
			if (!WheelMesh_FR_Asset) WheelMesh_FR_Asset = F1Front;
			if (!WheelMesh_RL_Asset) WheelMesh_RL_Asset = F1Rear;
			if (!WheelMesh_RR_Asset) WheelMesh_RR_Asset = F1Rear;
		}
	}

	if (!WheelMesh_FL_Asset || !WheelMesh_FR_Asset || !WheelMesh_RL_Asset || !WheelMesh_RR_Asset)
	{
		UStaticMesh* GemeraFL = UAssetManager::GetAssetByPath<UStaticMesh>("Meshes/Gemera/Gemera_Wheel_FL.obj");
		UStaticMesh* GemeraFR = UAssetManager::GetAssetByPath<UStaticMesh>("Meshes/Gemera/Gemera_Wheel_FR.obj");
		UStaticMesh* GemeraRL = UAssetManager::GetAssetByPath<UStaticMesh>("Meshes/Gemera/Gemera_Wheel_RL.obj");
		UStaticMesh* GemeraRR = UAssetManager::GetAssetByPath<UStaticMesh>("Meshes/Gemera/Gemera_Wheel_RR.obj");
		if (!WheelMesh_FL_Asset) WheelMesh_FL_Asset = GemeraFL;
		if (!WheelMesh_FR_Asset) WheelMesh_FR_Asset = GemeraFR;
		if (!WheelMesh_RL_Asset) WheelMesh_RL_Asset = GemeraRL;
		if (!WheelMesh_RR_Asset) WheelMesh_RR_Asset = GemeraRR;
	}

	if (!WheelMesh_FL_Asset || !WheelMesh_FR_Asset || !WheelMesh_RL_Asset || !WheelMesh_RR_Asset)
	{
		UStaticMesh* WheelMeshFront = UAssetManager::GetAssetByPath<UStaticMesh>("Meshes/Gemera/Gemera_Wheel_Front.obj");
		UStaticMesh* WheelMeshRear = UAssetManager::GetAssetByPath<UStaticMesh>("Meshes/Gemera/Gemera_Wheel_Rear.obj");
		if (!WheelMeshFront || !WheelMeshRear)
		{
			WheelMeshFront = UAssetManager::GetAssetByPath<UStaticMesh>("Meshes/Ferrari/Ferrari_Wheel_R.obj");
			WheelMeshRear = UAssetManager::GetAssetByPath<UStaticMesh>("Meshes/Ferrari/Ferrari_Wheel_L.obj");
		}
		if (WheelMeshFront && WheelMeshRear)
		{
			if (!WheelMesh_FL_Asset) WheelMesh_FL_Asset = WheelMeshFront;
			if (!WheelMesh_FR_Asset) WheelMesh_FR_Asset = WheelMeshFront;
			if (!WheelMesh_RL_Asset) WheelMesh_RL_Asset = WheelMeshRear;
			if (!WheelMesh_RR_Asset) WheelMesh_RR_Asset = WheelMeshRear;
		}
	}

	// 타이어 고무의 번들거림을 줄인다. OBJ 임포터는 .mtl의 Ns/Ks를 읽지 않아 모든 슬롯이 기본 광택이므로 여기서 낮춘다
	for (UStaticMesh* WheelAsset : { WheelMesh_FL_Asset, WheelMesh_FR_Asset, WheelMesh_RL_Asset, WheelMesh_RR_Asset })
	{
		if (!WheelAsset) continue;
		const FStaticMeshData& Data = WheelAsset->GetMeshData();
		for (int32 Slot = 0; Slot < Data.MaterialSlots.Num(); ++Slot)
		{
			if (Data.MaterialSlots[Slot].Name.find("tyre") == FString::npos) continue;
			if (UMaterial* TyreMaterial = Cast<UMaterial>(WheelAsset->GetMaterial(Slot)))
			{
				TyreMaterial->SpecularColor = FVector(0.08f, 0.08f, 0.08f);
				TyreMaterial->Shininess = 8.0f;
			}
		}
	}

	if (!WheelMesh_FL_Asset)
	{
		UStaticMesh* FallbackWheel = UAssetManager::GetAssetByPath<UStaticMesh>("Cylinder");
		if (!FallbackWheel)
		{
			FallbackWheel = UAssetManager::GetAssetByPath<UStaticMesh>("Cube");
		}
		WheelMesh_FL_Asset = FallbackWheel;
		WheelMesh_FR_Asset = FallbackWheel;
		WheelMesh_RL_Asset = FallbackWheel;
		WheelMesh_RR_Asset = FallbackWheel;
	}

	auto SetupWheelComp = [&](UStaticMeshComponent*& Comp, const char* Name, UStaticMesh* MeshAsset, const FVector& FallbackOffset)
	{
		Comp = CreateDefaultSubobject<UStaticMeshComponent>(Name);
		Comp->SetupAttachment(ChassisCollision);
		Comp->SetStaticMesh(MeshAsset);
		Comp->SetRelativeLocation(FallbackOffset);
		const bool bIsCyl = (!MeshAsset || MeshAsset->GetPath() == "Cylinder" || MeshAsset->GetPath() == "Cube");
		if (!bIsCyl)
		{
			Comp->SetRelativeScale3D(FVector(1.0f, 1.0f, 1.0f));
			Comp->SetRelativeRotation(FRotator(0.0f, 0.0f, 0.0f));
		}
		else
		{
			// Cylinder 기본(반경 1m, 높이 1m) -> 바퀴 반경 0.35m(지름 0.7m), 폭 0.2m 및 Roll 90도 정렬
			Comp->SetRelativeScale3D(FVector(0.35f, 0.35f, 0.20f));
			Comp->SetRelativeRotation(FRotator(0.0f, 0.0f, 90.0f));
		}
		Comp->BodyInstance.bSimulatePhysics = false;
		Comp->BodyInstance.CollisionEnabled = ECollisionEnabled::NoCollision;
	};

	SetupWheelComp(WheelMesh_FL, "Wheel_FL", WheelMesh_FL_Asset, FVector(1.483f, -0.842f, -0.2f));
	SetupWheelComp(WheelMesh_FR, "Wheel_FR", WheelMesh_FR_Asset, FVector(1.483f, 0.842f, -0.2f));
	SetupWheelComp(WheelMesh_RL, "Wheel_RL", WheelMesh_RL_Asset, FVector(-1.515f, -0.834f, -0.2f));
	SetupWheelComp(WheelMesh_RR, "Wheel_RR", WheelMesh_RR_Asset, FVector(-1.515f, 0.834f, -0.2f));

	// 6. 오디오 컴포넌트 (엔진 RPM / 풍절음 / 타이어 슬립)
	EngineAudioComponent = CreateDefaultSubobject<UAudioComponent>("EngineAudio");
	EngineAudioComponent->SetupAttachment(ChassisCollision);
	EngineAudioComponent->bAllowSpatialization = false;
	EngineAudioComponent->bLooping = true;
	EngineAudioComponent->SetAutoActivate(false);

	WindAudioComponent = CreateDefaultSubobject<UAudioComponent>("WindAudio");
	WindAudioComponent->SetupAttachment(ChassisCollision);
	WindAudioComponent->bAllowSpatialization = false;
	WindAudioComponent->bLooping = true;
	WindAudioComponent->SetAutoActivate(false);

	TireAudioComponent = CreateDefaultSubobject<UAudioComponent>("TireAudio");
	TireAudioComponent->SetupAttachment(ChassisCollision);
	TireAudioComponent->bAllowSpatialization = false;
	TireAudioComponent->bLooping = true;
	TireAudioComponent->SetAutoActivate(false);

	// 7. 파티클 컴포넌트 (후륜 타이어 연기)
	TireSmokeRL = CreateDefaultSubobject<UParticleSystemComponent>("TireSmoke_RL");
	TireSmokeRL->SetupAttachment(ChassisCollision);
	TireSmokeRL->SetAutoActivate(false);

	TireSmokeRR = CreateDefaultSubobject<UParticleSystemComponent>("TireSmoke_RR");
	TireSmokeRR->SetupAttachment(ChassisCollision);
	TireSmokeRR->SetAutoActivate(false);
}

AWheeledVehiclePawn::~AWheeledVehiclePawn()
{
	StopVehicleAudio();
	DestroyTelemetryHUD();
}

void AWheeledVehiclePawn::BeginPlay()
{
	Super::BeginPlay();

	if (CameraComponent)
	{
		BaseFieldOfView = CameraComponent->GetFieldOfView();
	}
	if (SpringArmComponent)
	{
		BaseTargetArmLength = SpringArmComponent->TargetArmLength;
		BaseSocketOffset = SpringArmComponent->SocketOffset;
		SmoothedSocketOffset = BaseSocketOffset;
		SpringArmComponent->bEnableCameraRotationLag = true;
		SpringArmComponent->CameraRotationLagSpeed = 12.0f;
		SpringArmComponent->bEnableCameraLag = true;
		SpringArmComponent->CameraLagSpeed = 20.0f;
		SpringArmComponent->CameraLagMaxDistance = 0.55f;
	}

	ElapsedSessionTime = 0.0f;
	BuffetingTime = 0.0f;

	if (GetWorld() && GetWorld()->IsGameWorld())
	{
		if (bEnableLapTimer)
		{
			InitLapTimer();
		}
		if (bShowTelemetryHUD)
		{
			CreateTelemetryHUD();
		}
		if (bEnableVehicleAudio)
		{
			InitVehicleAudio();
		}
		if (bEnableVehicleParticles)
		{
			InitVehicleParticles();
		}
	}
}

void AWheeledVehiclePawn::EndPlay()
{
	StopVehicleAudio();
	DestroyTelemetryHUD();
	if (CameraComponent)
	{
		CameraComponent->SetFieldOfView(BaseFieldOfView);
	}
	if (SpringArmComponent)
	{
		SpringArmComponent->TargetArmLength = BaseTargetArmLength;
		SpringArmComponent->SocketOffset = BaseSocketOffset;
	}
	Super::EndPlay();
}

void AWheeledVehiclePawn::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	// 바퀴 비주얼 컴포넌트 실시간 동기화 (서스펜션 상하 스트로크 + 조향각 + 롤링 회전)
	if (VehicleMovement)
	{
		// 실린더 메시는 중심축이 Z축(위쪽)에 있으므로, Roll 90도 회전을 합성하여 차량 좌우(Y축) 회전축과 일치시킴
		const FQuat CylinderOrientation = FRotator(0.0f, 0.0f, 90.0f).Quaternion();

		for (int32 i = 0; i < 4; ++i)
		{
			if (UStaticMeshComponent* WheelComp = GetWheelMesh(i))
			{
				const FTransform LocalWheelPose = VehicleMovement->GetWheelLocalTransform(i);
				WheelComp->SetRelativeLocation(LocalWheelPose.Location);

				const bool bIsCylinderFallback = (WheelComp->GetStaticMesh() && 
					(WheelComp->GetStaticMesh()->GetPath() == "Cylinder" || WheelComp->GetStaticMesh()->GetPath() == "Cube"));

				if (!bIsCylinderFallback)
				{
					// 커스텀 휠 OBJ(Gemera, Ferrari 등)는 이미 Y축이 회전축이며 전진/조향 축에 정렬되어 있으므로 포즈 회전을 그대로 적용
					const FQuat TotalWheelRot = LocalWheelPose.Rotation * WheelMeshRotationOffset.Quaternion();
					WheelComp->SetRelativeRotation(TotalWheelRot.ToFRotator());
				}
				else
				{
					const FQuat TotalWheelRot = LocalWheelPose.Rotation * CylinderOrientation;
					WheelComp->SetRelativeRotation(TotalWheelRot.ToFRotator());
				}
			}
		}

		// 속도 비례 동적 FOV 및 G-Force/풍압 카메라 모션
		const float SpeedKmH = std::abs(VehicleMovement->GetForwardSpeedKmH());
		const float SpeedAlpha = FMath::Clamp(SpeedKmH / 300.0f, 0.0f, 1.0f);

		if (CameraComponent && bEnableDynamicFOV)
		{
			const float TargetFOV = BaseFieldOfView + HighSpeedFOVBoost * std::pow(SpeedAlpha, 1.15f);
			const float NextFOV = FMath::FInterpTo(CameraComponent->GetFieldOfView(), TargetFOV, DeltaTime, 6.0f);
			CameraComponent->SetFieldOfView(NextFOV);
		}

		if (SpringArmComponent && bEnableGForceCamera)
		{
			const float LongG = VehicleMovement->GetLongitudinalG();
			const float LatG = VehicleMovement->GetLateralG();

			const float TargetArmLen = BaseTargetArmLength + FMath::Clamp(LongG * 0.25f, -0.45f, 0.55f);
			SpringArmComponent->TargetArmLength = FMath::FInterpTo(SpringArmComponent->TargetArmLength, TargetArmLen, DeltaTime, 8.0f);

			FVector TargetSocket = BaseSocketOffset;
			TargetSocket.Y += FMath::Clamp(-LatG * 0.14f, -0.45f, 0.45f);
			// 떨림은 보간한 값에 얹기만 한다. 떨림이 섞인 값을 다시 보간하면 누적돼 설정보다 몇 배 크게 흔들린다
			SmoothedSocketOffset = FMath::VInterpTo(SmoothedSocketOffset, TargetSocket, DeltaTime, 10.0f);
			FVector ShakenSocket = SmoothedSocketOffset;

			const float BuffetAlpha = FMath::Clamp((SpeedKmH - 130.0f) / 170.0f, 0.0f, 1.0f);
			const int32 GravelWheels = VehicleMovement->GetGravelWheelCount();
			const float GravelAlpha = (GravelWheels > 0)
				? (static_cast<float>(GravelWheels) / 4.0f) * FMath::Clamp(SpeedKmH / 30.0f, 0.0f, 1.0f)
				: 0.0f;

			if (BuffetAlpha > 0.01f || GravelAlpha > 0.01f)
			{
				BuffetingTime += DeltaTime * (18.0f + 16.0f * BuffetAlpha + 36.0f * GravelAlpha);
				const float ShakeY = 0.010f * BuffetAlpha + 0.018f * GravelAlpha;
				const float ShakeZ = 0.008f * BuffetAlpha + 0.014f * GravelAlpha;
				ShakenSocket.Y += std::sin(BuffetingTime * 1.7f) * ShakeY;
				ShakenSocket.Z += std::cos(BuffetingTime * 2.3f) * ShakeZ;
			}

			SpringArmComponent->SocketOffset = ShakenSocket;
		}

		if (bEnableLapTimer && GetWorld() && GetWorld()->IsGameWorld())
		{
			UpdateLapTimer(DeltaTime);
		}
		UpdateTelemetryHUD(DeltaTime);
		if (bEnableVehicleAudio)
		{
			UpdateVehicleAudio(DeltaTime);
		}
		if (bEnableVehicleParticles)
		{
			UpdateVehicleParticles(DeltaTime);
		}
	}
}

void AWheeledVehiclePawn::CreateTelemetryHUD()
{
	if (TelemetryWidget || !GetWorld())
	{
		return;
	}

	TelemetryWidget = NewObject<UUserWidget>(nullptr);
	if (!TelemetryWidget)
	{
		return;
	}
	TelemetryWidget->SetOwningWorld(GetWorld());
	TelemetryWidget->Visibility = static_cast<int32>(EUIVisibility::HitTestInvisible);

	UCanvasPanel* RootCanvas = NewObject<UCanvasPanel>(nullptr);
	TelemetryWidget->SetRootWidget(RootCanvas);

	// 우하단 F1 텔레메트리 카드 (360 x 176 px @ 1080p 기준)
	UImage* CardBg = NewObject<UImage>(nullptr);
	CardBg->Anchors = FVector4(1.0f, 1.0f, 1.0f, 1.0f);
	CardBg->AlignmentX = 1.0f;
	CardBg->AlignmentY = 1.0f;
	CardBg->Offsets = FVector4(-32.0f, -32.0f, 360.0f, 176.0f);
	CardBg->ColorAndOpacity = FLinearColor(0.05f, 0.06f, 0.08f, 0.82f);
	RootCanvas->AddChild(CardBg);

	// 상단 페라리 레드 액센트 바
	UImage* AccentLine = NewObject<UImage>(nullptr);
	AccentLine->Anchors = FVector4(1.0f, 1.0f, 1.0f, 1.0f);
	AccentLine->AlignmentX = 1.0f;
	AccentLine->AlignmentY = 1.0f;
	AccentLine->Offsets = FVector4(-32.0f, -204.0f, 360.0f, 4.0f);
	AccentLine->ColorAndOpacity = FLinearColor(0.86f, 0.05f, 0.08f, 0.95f);
	RootCanvas->AddChild(AccentLine);

	// 기어 배경 박스
	UImage* GearBg = NewObject<UImage>(nullptr);
	GearBg->Anchors = FVector4(1.0f, 1.0f, 1.0f, 1.0f);
	GearBg->AlignmentX = 1.0f;
	GearBg->AlignmentY = 1.0f;
	GearBg->Offsets = FVector4(-48.0f, -124.0f, 68.0f, 68.0f);
	GearBg->ColorAndOpacity = FLinearColor(0.11f, 0.13f, 0.17f, 0.95f);
	RootCanvas->AddChild(GearBg);

	// 속도 수치 (KM/H)
	SpeedText = NewObject<UTextBlock>(nullptr);
	SpeedText->bAutoSize = false;
	SpeedText->Anchors = FVector4(1.0f, 1.0f, 1.0f, 1.0f);
	SpeedText->AlignmentX = 0.0f;
	SpeedText->AlignmentY = 0.0f;
	SpeedText->Offsets = FVector4(-372.0f, -192.0f, 175.0f, 54.0f);
	SpeedText->FontSize = 48.0f;
	SpeedText->ColorAndOpacity = FLinearColor(0.98f, 0.98f, 1.0f, 1.0f);
	SpeedText->SetText("0");
	RootCanvas->AddChild(SpeedText);

	UTextBlock* UnitText = NewObject<UTextBlock>(nullptr);
	UnitText->bAutoSize = false;
	UnitText->Anchors = FVector4(1.0f, 1.0f, 1.0f, 1.0f);
	UnitText->AlignmentX = 0.0f;
	UnitText->AlignmentY = 0.0f;
	UnitText->Offsets = FVector4(-372.0f, -142.0f, 80.0f, 22.0f);
	UnitText->FontSize = 14.0f;
	UnitText->ColorAndOpacity = FLinearColor(0.65f, 0.70f, 0.78f, 1.0f);
	UnitText->SetText("KM/H");
	RootCanvas->AddChild(UnitText);

	// 기어 텍스트
	GearText = NewObject<UTextBlock>(nullptr);
	GearText->bAutoSize = false;
	GearText->Anchors = FVector4(1.0f, 1.0f, 1.0f, 1.0f);
	GearText->AlignmentX = 0.0f;
	GearText->AlignmentY = 0.0f;
	GearText->Offsets = FVector4(-116.0f, -186.0f, 68.0f, 60.0f);
	GearText->FontSize = 46.0f;
	GearText->Justification = 1;
	GearText->ColorAndOpacity = FLinearColor(1.0f, 0.82f, 0.18f, 1.0f);
	GearText->SetText("1");
	RootCanvas->AddChild(GearText);

	// RPM 프로그레스 바
	RPMBar = NewObject<UProgressBar>(nullptr);
	RPMBar->Anchors = FVector4(1.0f, 1.0f, 1.0f, 1.0f);
	RPMBar->AlignmentX = 0.0f;
	RPMBar->AlignmentY = 0.0f;
	RPMBar->Offsets = FVector4(-372.0f, -114.0f, 324.0f, 14.0f);
	RPMBar->BackgroundColor = FLinearColor(0.12f, 0.14f, 0.18f, 0.95f);
	RPMBar->BorderPadding = FVector(1.0f, 1.0f, 0.0f);
	RPMBar->SetFillColorAndOpacity(FLinearColor(0.18f, 0.88f, 0.40f, 1.0f));
	RPMBar->SetPercent(0.25f);
	RootCanvas->AddChild(RPMBar);

	// RPM & G-Force 정보
	RPMText = NewObject<UTextBlock>(nullptr);
	RPMText->bAutoSize = false;
	RPMText->Anchors = FVector4(1.0f, 1.0f, 1.0f, 1.0f);
	RPMText->AlignmentX = 0.0f;
	RPMText->AlignmentY = 0.0f;
	RPMText->Offsets = FVector4(-372.0f, -94.0f, 150.0f, 22.0f);
	RPMText->FontSize = 15.0f;
	RPMText->ColorAndOpacity = FLinearColor(0.82f, 0.86f, 0.92f, 1.0f);
	RPMText->SetText("4200 RPM");
	RootCanvas->AddChild(RPMText);

	GForceText = NewObject<UTextBlock>(nullptr);
	GForceText->bAutoSize = false;
	GForceText->Anchors = FVector4(1.0f, 1.0f, 1.0f, 1.0f);
	GForceText->AlignmentX = 0.0f;
	GForceText->AlignmentY = 0.0f;
	GForceText->Offsets = FVector4(-222.0f, -94.0f, 174.0f, 22.0f);
	GForceText->FontSize = 15.0f;
	GForceText->Justification = 2;
	GForceText->ColorAndOpacity = FLinearColor(0.68f, 0.84f, 1.0f, 1.0f);
	GForceText->SetText("0.0G LAT");
	RootCanvas->AddChild(GForceText);

	// 주행 시간 & TCS/DRS 상태
	TimerText = NewObject<UTextBlock>(nullptr);
	TimerText->bAutoSize = false;
	TimerText->Anchors = FVector4(1.0f, 1.0f, 1.0f, 1.0f);
	TimerText->AlignmentX = 0.0f;
	TimerText->AlignmentY = 0.0f;
	TimerText->Offsets = FVector4(-372.0f, -68.0f, 160.0f, 20.0f);
	TimerText->FontSize = 14.0f;
	TimerText->ColorAndOpacity = FLinearColor(0.75f, 0.78f, 0.84f, 1.0f);
	TimerText->SetText("00:00.0");
	RootCanvas->AddChild(TimerText);

	StatusText = NewObject<UTextBlock>(nullptr);
	StatusText->bAutoSize = false;
	StatusText->Anchors = FVector4(1.0f, 1.0f, 1.0f, 1.0f);
	StatusText->AlignmentX = 0.0f;
	StatusText->AlignmentY = 0.0f;
	StatusText->Offsets = FVector4(-210.0f, -68.0f, 162.0f, 20.0f);
	StatusText->FontSize = 14.0f;
	StatusText->Justification = 2;
	StatusText->ColorAndOpacity = FLinearColor(0.35f, 0.90f, 0.45f, 1.0f);
	StatusText->SetText("TCS ON");
	RootCanvas->AddChild(StatusText);

	// 스로틀 / 브레이크 입력 바
	ThrottleBar = NewObject<UProgressBar>(nullptr);
	ThrottleBar->Anchors = FVector4(1.0f, 1.0f, 1.0f, 1.0f);
	ThrottleBar->AlignmentX = 0.0f;
	ThrottleBar->AlignmentY = 0.0f;
	ThrottleBar->Offsets = FVector4(-372.0f, -44.0f, 158.0f, 8.0f);
	ThrottleBar->BackgroundColor = FLinearColor(0.12f, 0.14f, 0.16f, 0.95f);
	ThrottleBar->BorderPadding = FVector(0.0f, 0.0f, 0.0f);
	ThrottleBar->SetFillColorAndOpacity(FLinearColor(0.16f, 0.92f, 0.44f, 1.0f));
	ThrottleBar->SetPercent(0.0f);
	RootCanvas->AddChild(ThrottleBar);

	BrakeBar = NewObject<UProgressBar>(nullptr);
	BrakeBar->Anchors = FVector4(1.0f, 1.0f, 1.0f, 1.0f);
	BrakeBar->AlignmentX = 0.0f;
	BrakeBar->AlignmentY = 0.0f;
	BrakeBar->Offsets = FVector4(-206.0f, -44.0f, 158.0f, 8.0f);
	BrakeBar->BackgroundColor = FLinearColor(0.12f, 0.14f, 0.16f, 0.95f);
	BrakeBar->BorderPadding = FVector(0.0f, 0.0f, 0.0f);
	BrakeBar->SetFillColorAndOpacity(FLinearColor(0.95f, 0.22f, 0.22f, 1.0f));
	BrakeBar->SetPercent(0.0f);
	RootCanvas->AddChild(BrakeBar);

	// 우상단 랩타임 카드
	if (bEnableLapTimer)
	{
		UImage* LapBg = NewObject<UImage>(nullptr);
		LapBg->Anchors = FVector4(1.0f, 0.0f, 1.0f, 0.0f);
		LapBg->AlignmentX = 1.0f;
		LapBg->AlignmentY = 0.0f;
		LapBg->Offsets = FVector4(-32.0f, 32.0f, 260.0f, 100.0f);
		LapBg->ColorAndOpacity = FLinearColor(0.05f, 0.06f, 0.08f, 0.82f);
		RootCanvas->AddChild(LapBg);

		auto AddLapLine = [&](float Top, float Height, float FontSize, const FLinearColor& Color)
		{
			UTextBlock* Text = NewObject<UTextBlock>(nullptr);
			Text->bAutoSize = false;
			Text->Anchors = FVector4(1.0f, 0.0f, 1.0f, 0.0f);
			Text->AlignmentX = 0.0f;
			Text->AlignmentY = 0.0f;
			Text->Offsets = FVector4(-278.0f, Top, 232.0f, Height);
			Text->FontSize = FontSize;
			Text->ColorAndOpacity = Color;
			RootCanvas->AddChild(Text);
			return Text;
		};
		LapText = AddLapLine(42.0f, 30.0f, 24.0f, FLinearColor(0.98f, 0.98f, 1.0f, 1.0f));
		LastLapText = AddLapLine(78.0f, 22.0f, 15.0f, FLinearColor(0.82f, 0.86f, 0.92f, 1.0f));
		BestLapText = AddLapLine(102.0f, 22.0f, 15.0f, FLinearColor(0.78f, 0.45f, 1.0f, 1.0f));
	}

	TelemetryWidget->OnDestroyed = [this]()
	{
		TelemetryWidget = nullptr;
		SpeedText = nullptr;
		GearText = nullptr;
		RPMText = nullptr;
		GForceText = nullptr;
		TimerText = nullptr;
		StatusText = nullptr;
		RPMBar = nullptr;
		ThrottleBar = nullptr;
		BrakeBar = nullptr;
		LapText = nullptr;
		LastLapText = nullptr;
		BestLapText = nullptr;
	};

	TelemetryWidget->AddToViewport(50);
	if (!TelemetryWidget->IsInViewport())
	{
		TelemetryWidget->Destroy();
	}
}

void AWheeledVehiclePawn::UpdateTelemetryHUD(float DeltaTime)
{
	if (!TelemetryWidget || !VehicleMovement)
	{
		return;
	}

	ElapsedSessionTime += DeltaTime;
	const int32 SpeedKmH = static_cast<int32>(std::round(std::abs(VehicleMovement->GetForwardSpeedKmH())));
	const int32 Gear = VehicleMovement->GetCurrentGear();
	const float RPM = VehicleMovement->GetEngineRPM();
	const float MaxRPM = std::max(VehicleMovement->GetMaxRPM(), 1000.0f);
	const float RPMRatio = FMath::Clamp(RPM / MaxRPM, 0.0f, 1.0f);

	char Buf[64];
	if (SpeedText)
	{
		std::snprintf(Buf, sizeof(Buf), "%d", SpeedKmH);
		SpeedText->SetText(Buf);
	}

	if (GearText)
	{
		if (Gear < 0)
		{
			GearText->SetText("R");
			GearText->ColorAndOpacity = FLinearColor(0.95f, 0.30f, 0.30f, 1.0f);
		}
		else if (Gear == 0)
		{
			GearText->SetText("N");
			GearText->ColorAndOpacity = FLinearColor(0.85f, 0.85f, 0.85f, 1.0f);
		}
		else
		{
			std::snprintf(Buf, sizeof(Buf), "%d", Gear);
			GearText->SetText(Buf);
			GearText->ColorAndOpacity = (RPMRatio > 0.88f)
				? FLinearColor(0.98f, 0.25f, 0.32f, 1.0f)
				: FLinearColor(1.0f, 0.82f, 0.18f, 1.0f);
		}
	}

	if (RPMBar)
	{
		RPMBar->SetPercent(RPMRatio);
		if (RPMRatio > 0.88f)
		{
			RPMBar->SetFillColorAndOpacity(FLinearColor(0.95f, 0.16f, 0.28f, 1.0f));
		}
		else if (RPMRatio > 0.68f)
		{
			RPMBar->SetFillColorAndOpacity(FLinearColor(0.98f, 0.72f, 0.14f, 1.0f));
		}
		else
		{
			RPMBar->SetFillColorAndOpacity(FLinearColor(0.18f, 0.88f, 0.40f, 1.0f));
		}
	}

	if (RPMText)
	{
		std::snprintf(Buf, sizeof(Buf), "%d RPM", static_cast<int32>(std::round(RPM)));
		RPMText->SetText(Buf);
	}

	if (GForceText)
	{
		std::snprintf(Buf, sizeof(Buf), "%.1fG LAT  %.1fG", std::abs(VehicleMovement->GetLateralG()), VehicleMovement->GetLongitudinalG());
		GForceText->SetText(Buf);
	}

	if (TimerText)
	{
		const int32 TotalTenths = static_cast<int32>(ElapsedSessionTime * 10.0f);
		const int32 Mins = TotalTenths / 600;
		const int32 Secs = (TotalTenths / 10) % 60;
		const int32 Tenths = TotalTenths % 10;
		std::snprintf(Buf, sizeof(Buf), "%02d:%02d.%d", Mins, Secs, Tenths);
		TimerText->SetText(Buf);
	}

	if (LapText && LastLapText && BestLapText)
	{
		char Time[32];
		if (bLapRunning)
		{
			FormatLapTime(CurrentLapTime, Time, sizeof(Time));
			std::snprintf(Buf, sizeof(Buf), "LAP %d   %s", CurrentLap, Time);
		}
		else
		{
			std::snprintf(Buf, sizeof(Buf), "LAP -   -:--.---");
		}
		LapText->SetText(Buf);

		FormatLapTime(LastLapTime, Time, sizeof(Time));
		std::snprintf(Buf, sizeof(Buf), "LAST  %s", Time);
		LastLapText->SetText(Buf);

		FormatLapTime(BestLapTime, Time, sizeof(Time));
		std::snprintf(Buf, sizeof(Buf), "BEST  %s", Time);
		BestLapText->SetText(Buf);
	}

	if (StatusText)
	{
		if (VehicleMovement->IsOnGravel())
		{
			StatusText->SetText("GRAVEL");
			StatusText->ColorAndOpacity = FLinearColor(0.95f, 0.62f, 0.22f, 1.0f);
		}
		else if (VehicleMovement->IsDRSActive())
		{
			StatusText->SetText("DRS OPEN");
			StatusText->ColorAndOpacity = FLinearColor(0.15f, 0.90f, 1.0f, 1.0f);
		}
		else if (!VehicleMovement->bEnableTractionControl)
		{
			StatusText->SetText("TCS OFF");
			StatusText->ColorAndOpacity = FLinearColor(0.60f, 0.64f, 0.70f, 1.0f);
		}
		else if (VehicleMovement->IsTCActive())
		{
			StatusText->SetText("TCS ACTIVE");
			StatusText->ColorAndOpacity = FLinearColor(1.0f, 0.75f, 0.16f, 1.0f);
		}
		else
		{
			StatusText->SetText("TCS READY");
			StatusText->ColorAndOpacity = FLinearColor(0.35f, 0.90f, 0.45f, 1.0f);
		}
	}

	if (ThrottleBar)
	{
		ThrottleBar->SetPercent(FMath::Clamp(VehicleMovement->GetThrottleInput(), 0.0f, 1.0f));
	}
	if (BrakeBar)
	{
		BrakeBar->SetPercent(FMath::Clamp(VehicleMovement->GetBrakeInput(), 0.0f, 1.0f));
	}
}

void AWheeledVehiclePawn::DestroyTelemetryHUD()
{
	if (TelemetryWidget)
	{
		UUserWidget* WidgetToDestroy = TelemetryWidget;
		TelemetryWidget = nullptr;
		WidgetToDestroy->Destroy();
	}
}

void AWheeledVehiclePawn::InitVehicleAudio()
{
	if (EngineAudioComponent)
	{
		EngineAudioComponent->SetSound(GetOrCreateF1EngineWave());
		EngineAudioComponent->SetVolumeMultiplier(0.18f);
		EngineAudioComponent->SetPitchMultiplier(0.75f);
		EngineAudioComponent->Play();
	}
	if (WindAudioComponent)
	{
		WindAudioComponent->SetSound(GetOrCreateAeroWindWave());
		WindAudioComponent->SetVolumeMultiplier(0.0f);
		WindAudioComponent->SetPitchMultiplier(0.85f);
		WindAudioComponent->Play();
	}
	if (TireAudioComponent)
	{
		TireAudioComponent->SetSound(GetOrCreateTireSkidWave());
		TireAudioComponent->SetVolumeMultiplier(0.0f);
		TireAudioComponent->SetPitchMultiplier(0.95f);
		TireAudioComponent->Play();
	}
}

void AWheeledVehiclePawn::UpdateVehicleAudio(float DeltaTime)
{
	if (!VehicleMovement)
	{
		return;
	}

	const float RPM = VehicleMovement->GetEngineRPM();
	const float MaxRPM = std::max(VehicleMovement->GetMaxRPM(), 1000.0f);
	const float RPMNorm = FMath::Clamp(RPM / MaxRPM, 0.0f, 1.0f);
	const float Throttle = std::abs(VehicleMovement->GetThrottleInput());
	const float SpeedKmH = std::abs(VehicleMovement->GetForwardSpeedKmH());

	if (EngineAudioComponent)
	{
		const float TargetPitch = FMath::Lerp(0.65f, 2.45f, RPMNorm);
		const float TargetVol = FMath::Lerp(0.16f, 0.40f, std::max(RPMNorm * 0.7f, Throttle));
		EngineAudioComponent->SetPitchMultiplier(TargetPitch);
		EngineAudioComponent->SetVolumeMultiplier(TargetVol);
	}

	if (WindAudioComponent)
	{
		const float WindAlpha = FMath::Clamp((SpeedKmH - 70.0f) / 230.0f, 0.0f, 1.0f);
		WindAudioComponent->SetVolumeMultiplier(WindAlpha * 0.34f);
		WindAudioComponent->SetPitchMultiplier(0.85f + WindAlpha * 0.65f);
	}

	if (TireAudioComponent)
	{
		float MaxSlipMetric = 0.0f;
		if (SpeedKmH > 12.0f)
		{
			for (int32 i = 0; i < 4; ++i)
			{
				const FWheelSimulationState& WS = VehicleMovement->GetWheelState(i);
				if (WS.bInContact)
				{
					const float LatSlip = FMath::Clamp((std::abs(WS.SlipAngleRad) - 0.10f) / 0.20f, 0.0f, 1.0f);
					const float LongSlip = FMath::Clamp((std::abs(WS.SlipRatio) - 0.10f) / 0.25f, 0.0f, 1.0f);
					const float GravelScrub = WS.bOnGravel ? FMath::Clamp(SpeedKmH / 90.0f, 0.0f, 0.65f) : 0.0f;
					MaxSlipMetric = std::max(MaxSlipMetric, std::max({ LatSlip, LongSlip, GravelScrub }));
				}
			}
		}
		TireAudioComponent->SetVolumeMultiplier(MaxSlipMetric * 0.32f);
		TireAudioComponent->SetPitchMultiplier(0.92f + MaxSlipMetric * 0.30f);
	}
}

void AWheeledVehiclePawn::StopVehicleAudio()
{
	if (EngineAudioComponent) EngineAudioComponent->Stop();
	if (WindAudioComponent) WindAudioComponent->Stop();
	if (TireAudioComponent) TireAudioComponent->Stop();
}

void AWheeledVehiclePawn::InitVehicleParticles()
{
	UParticleSystem* SmokeSys = GetOrCreateTireSmokeTemplate();

	if (TireSmokeRL && SmokeSys)
	{
		TireSmokeRL->InitializeSystem(SmokeSys);
		TireSmokeRL->DeactivateSystem();
	}
	if (TireSmokeRR && SmokeSys)
	{
		TireSmokeRR->InitializeSystem(SmokeSys);
		TireSmokeRR->DeactivateSystem();
	}
	bTireSmokeActiveRL = false;
	bTireSmokeActiveRR = false;
}

void AWheeledVehiclePawn::UpdateVehicleParticles(float DeltaTime)
{
	if (!VehicleMovement)
	{
		return;
	}

	const float Speed = std::abs(VehicleMovement->GetForwardSpeed());
	const FVehicleParams& Params = VehicleMovement->GetVehicleParams();

	auto UpdateWheelSmoke = [&](UParticleSystemComponent* PSC, int32 WheelIdx, bool& bActiveFlag)
	{
		if (!PSC) return;
		const FWheelSimulationState& WS = VehicleMovement->GetWheelState(WheelIdx);
		const float WheelRadius = Params.GetWheel(WheelIdx).Radius;
		FVector ContactOffset = WS.LocalPose.Location;
		ContactOffset.Z -= WheelRadius * 0.85f;
		PSC->SetRelativeLocation(ContactOffset);

		const bool bShouldSmoke = WS.bInContact && Speed > 4.0f &&
			(WS.bOnGravel || std::abs(WS.SlipAngleRad) > 0.14f || std::abs(WS.SlipRatio) > 0.12f || VehicleMovement->GetHandbrakeInput());
		// AutoActivate를 끈 컴포넌트는 Tick도 꺼져 있다. ActivateSystem만 부르면 방출 플래그만 켜지므로 Activate를 쓴다.
		// Deactivate는 방출만 멈추고 Tick은 유지해, 남은 파티클이 수명대로 사라진다.
		if (bShouldSmoke && !bActiveFlag)
		{
			PSC->Activate(false);
			bActiveFlag = true;
		}
		else if (!bShouldSmoke && bActiveFlag)
		{
			PSC->Deactivate();
			bActiveFlag = false;
		}
	};

	UpdateWheelSmoke(TireSmokeRL, 2, bTireSmokeActiveRL);
	UpdateWheelSmoke(TireSmokeRR, 3, bTireSmokeActiveRR);
}

void AWheeledVehiclePawn::InitLapTimer()
{
	constexpr float LineAhead = 5.0f; // 출발 위치에서 결승선까지의 거리 (m)

	FVector Forward = GetActorRotation().Quaternion().GetForwardVector();
	Forward.Z = 0.0f;
	LapLineForward = Forward.Normalized();
	LapLineOrigin = GetActorLocation() + LapLineForward * LineAhead;
	LapPrevLocation = GetActorLocation();

	bLapRunning = false;
	CurrentLap = 0;
	CurrentLapTime = 0.0f;
	LapDistance = 0.0f;
	LastLapTime = 0.0f;
	BestLapTime = LoadBestLapTime(LapLineOrigin);
}

void AWheeledVehiclePawn::UpdateLapTimer(float DeltaTime)
{
	constexpr float LineHalfWidth = 15.0f;
	constexpr float LineHalfHeight = 5.0f;  // 입체 교차로의 위아래 도로를 구분한다
	constexpr float MaxJump = 20.0f;        // 한 프레임에 이보다 멀리 가면 순간이동으로 본다

	const FVector Location = GetActorLocation();
	const float Moved = (Location - LapPrevLocation).Size();
	const float PrevAlong = FVector::Dot(LapPrevLocation - LapLineOrigin, LapLineForward);
	LapPrevLocation = Location;

	if (Moved > MaxJump)
	{
		bLapRunning = false;
		return;
	}
	if (bLapRunning)
	{
		CurrentLapTime += DeltaTime;
		LapDistance += Moved;
	}

	// 결승선을 뒤에서 앞으로 넘었는지 확인
	const FVector Offset = Location - LapLineOrigin;
	const float Along = FVector::Dot(Offset, LapLineForward);
	if (!(PrevAlong < 0.0f && Along >= 0.0f))
	{
		return;
	}
	const float Lateral = std::abs(Offset.X * LapLineForward.Y - Offset.Y * LapLineForward.X);
	if (Lateral > LineHalfWidth || std::abs(Offset.Z) > LineHalfHeight)
	{
		return;
	}

	// 프레임 중간에 선을 넘은 시점까지 보간해 프레임 속도와 상관없이 기록한다
	const float Overshoot = DeltaTime * Along / (Along - PrevAlong);
	if (!bLapRunning)
	{
		bLapRunning = true;
		CurrentLap = 1;
	}
	else if (LapDistance >= MinLapDistance)
	{
		LastLapTime = CurrentLapTime - Overshoot;
		if (BestLapTime <= 0.0f || LastLapTime < BestLapTime)
		{
			BestLapTime = LastLapTime;
		}
		AppendLapRecord(LapLineOrigin, CurrentLap, LastLapTime);
		KRS_LOG(Info, "[Lap] lap {} finished in {} s", CurrentLap, LastLapTime);
		++CurrentLap;
	}
	else
	{
		return; // 한 바퀴를 돌지 않고 다시 넘은 것은 무시하고 계속 잰다
	}
	CurrentLapTime = Overshoot;
	LapDistance = 0.0f;
}

UStaticMeshComponent* AWheeledVehiclePawn::GetWheelMesh(int32 Index) const
{
	switch (Index)
	{
	case 0: return WheelMesh_FL;
	case 1: return WheelMesh_FR;
	case 2: return WheelMesh_RL;
	case 3: return WheelMesh_RR;
	default: return nullptr;
	}
}

UPawnMovementComponent* AWheeledVehiclePawn::GetMovementComponent() const
{
	return VehicleMovement;
}

void AWheeledVehiclePawn::SetThrottleInput(float Val)
{
	if (VehicleMovement)
	{
		VehicleMovement->SetThrottleInput(Val);
	}
}

void AWheeledVehiclePawn::SetSteeringInput(float Val)
{
	if (VehicleMovement)
	{
		VehicleMovement->SetSteeringInput(Val);
	}
}

void AWheeledVehiclePawn::SetBrakeInput(float Val)
{
	if (VehicleMovement)
	{
		VehicleMovement->SetBrakeInput(Val);
	}
}

void AWheeledVehiclePawn::SetHandbrakeInput(bool bVal)
{
	if (VehicleMovement)
	{
		VehicleMovement->SetHandbrakeInput(bVal);
	}
}

void AWheeledVehiclePawn::ToggleDRS()
{
	if (VehicleMovement)
	{
		VehicleMovement->SetDRSRequested(!VehicleMovement->IsDRSRequested());
	}
}

void AWheeledVehiclePawn::ToggleTractionControl()
{
	if (VehicleMovement)
	{
		VehicleMovement->bEnableTractionControl = !VehicleMovement->bEnableTractionControl;
	}
}

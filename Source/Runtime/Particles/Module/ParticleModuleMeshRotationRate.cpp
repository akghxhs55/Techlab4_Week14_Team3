#include "EnginePCH.h"
#include "ParticleModuleMeshRotationRate.h"
#include "ParticleModuleTypeDataMesh.h"
#include "ParticleModuleTypeDataGpuMesh.h"
#include "Particles/ParticleLODLevel.h"
#include "Particles/ParticleMeshEmitterInstance.h"

UParticleModuleMeshRotationRate::UParticleModuleMeshRotationRate()
{
	bSpawnModule = true;

	StartRotationRate.Mode = EDistributionMode::Uniform;
	StartRotationRate.MinValue = FVector(-180.0f, -180.0f, -180.0f);
	StartRotationRate.MaxValue = FVector(180.0f, 180.0f, 180.0f);
	StartRotationRate.bLockAxes = false;

	BakeDistributions();
}

void UParticleModuleMeshRotationRate::BakeDistributions()
{
	StartRotationRate.Bake();
}

void UParticleModuleMeshRotationRate::Spawn(FParticleEmitterInstance& Owner, int32 Offset, float SpawnTime, FBaseParticle& Particle)
{
	// 메시 인스턴스는 TypeDataMesh만 만들므로 TypeData로 판별한다
	// GPU Mesh도 TypeDataMesh를 상속하지만 CPU 메시 회전 Payload는 없다
	UParticleModuleTypeDataBase* TypeData = Owner.CurrentLODLevel ? Owner.CurrentLODLevel->GetTypeDataModule() : nullptr;
	if (!Cast<UParticleModuleTypeDataMesh>(TypeData) || Cast<UParticleModuleTypeDataGpuMesh>(TypeData))
		return;

	FMeshRotationPayloadData& Payload = static_cast<FParticleMeshEmitterInstance&>(Owner).GetMeshRotationPayload(Particle);
	const FVector Rate = StartRotationRate.GetVectorValue(Owner.EmitterTime, Owner.RandomStream);
	Payload.RotationRate += Rate;
	Payload.RotationRateBase += Rate;
}

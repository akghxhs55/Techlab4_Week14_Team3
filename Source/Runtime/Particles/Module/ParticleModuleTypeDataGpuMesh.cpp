#include "EnginePCH.h"
#include "ParticleModuleTypeDataGpuMesh.h"
#include "Particles/ParticleGpuEmitterInstance.h"

FParticleEmitterInstance* UParticleModuleTypeDataGpuMesh::CreateInstance(UParticleEmitter* InEmitter, UParticleSystemComponent* InComponent)
{
	FParticleGpuEmitterInstance* Instance = new FParticleGpuEmitterInstance();
	Instance->Init(InEmitter, InComponent);
	return Instance;
}

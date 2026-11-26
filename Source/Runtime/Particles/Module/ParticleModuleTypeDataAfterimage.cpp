#include "EnginePCH.h"
#include "ParticleModuleTypeDataAfterimage.h"
#include "Particles/ParticleAfterimageEmitterInstance.h"

FParticleEmitterInstance* UParticleModuleTypeDataAfterimage::CreateInstance(UParticleEmitter* InEmitter, UParticleSystemComponent* InComponent)
{
	FParticleAfterimageEmitterInstance* Instance = new FParticleAfterimageEmitterInstance();
	Instance->Init(InEmitter, InComponent);
	return Instance;
}

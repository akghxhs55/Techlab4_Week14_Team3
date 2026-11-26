#pragma once

#include "Particles/Module/ParticleModuleTypeDataBase.h"
#include "ParticleModuleTypeDataAfterimage.generated.h"

UCLASS()
class UParticleModuleTypeDataAfterimage : public UParticleModuleTypeDataBase
{
	GENERATED_BODY()
public:
	FParticleEmitterInstance* CreateInstance(UParticleEmitter* InEmitter, UParticleSystemComponent* InComponent) override;
};

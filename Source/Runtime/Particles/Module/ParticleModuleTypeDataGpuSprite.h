#pragma once

#include "Particles/Module/ParticleModuleTypeDataBase.h"
#include "ParticleModuleTypeDataGpuSprite.generated.h"

UCLASS()
class UParticleModuleTypeDataGpuSprite : public UParticleModuleTypeDataBase
{
	GENERATED_BODY()
public:
	FParticleEmitterInstance* CreateInstance(UParticleEmitter* InEmitter, UParticleSystemComponent* InComponent) override;

	int32 GetMaxGpuParticles() const { return MaxGpuParticles; }

private:
	UPROPERTY(EditAnywhere, meta=(ClampMin=1))
	int32 MaxGpuParticles = 2048;
};

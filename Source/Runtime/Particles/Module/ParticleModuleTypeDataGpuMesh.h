#pragma once

#include "Particles/Module/ParticleModuleTypeDataMesh.h"
#include "ParticleModuleTypeDataGpuMesh.generated.h"

UCLASS()
class UParticleModuleTypeDataGpuMesh : public UParticleModuleTypeDataMesh
{
	GENERATED_BODY()
public:
	FParticleEmitterInstance* CreateInstance(UParticleEmitter* InEmitter, UParticleSystemComponent* InComponent) override;

	int32 GetMaxGpuParticles() const { return MaxGpuParticles; }

private:
	UPROPERTY(EditAnywhere, meta=(ClampMin=1))
	int32 MaxGpuParticles = 1024;
};

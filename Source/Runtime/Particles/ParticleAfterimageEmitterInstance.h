#pragma once

#include "Particles/ParticleEmitterInstance.h"
#include "Engine/ParticleSystemRender.h"

struct FParticleAfterimageEmitterInstance : public FParticleEmitterInstance
{
	FParticleAfterimageEmitterInstance() = default;
	~FParticleAfterimageEmitterInstance() override = default;

	bool Resize(int32 NewMaxActiveParticles) override;
	void KillParticles() override;
	void KillAllParticles();
	void UpdateDynamicData(TUniquePtr<FDynamicEmitterDataBase>& InOutData) const override;

protected:
	void PostSpawn(FBaseParticle* Particle, float SpawnTime) override;
	void UpdateBoundingBox(float DeltaTime) override;

private:
	TArray<TSharedPtr<FAfterimageSnapshot>> Snapshots;
};

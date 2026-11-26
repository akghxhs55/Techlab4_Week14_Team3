#pragma once

#include "Core/Types.h"
#include "Particles/ParticleModule.h"
#include "Particles/Distributions.h"
#include "ParticleModuleLocation.generated.h"

UCLASS()
class UParticleModuleLocation : public UParticleModule
{
	GENERATED_BODY()
public:
	UParticleModuleLocation();

	void Spawn(FParticleEmitterInstance& Owner, int32 Offset, float SpawnTime, FBaseParticle& Particle) override;
	void BakeDistributions() override;

	FVectorDistribution& GetStartLocation() { return StartLocation; }

	bool IsSamplingSkeletalMesh() const { return bSampleSkeletalMesh; }
	void SetSampleSkeletalMesh(bool bInSample) { bSampleSkeletalMesh = bInSample; }
	bool IsFilteringByDissolveHeight() const { return bFilterByDissolveHeight; }
	void SetFilterByDissolveHeight(bool bInFilter) { bFilterByDissolveHeight = bInFilter; }
	const FString& GetDissolveHeightParamName() const { return DissolveHeightParamName; }
	void SetDissolveHeightParamName(const FString& InName) { DissolveHeightParamName = InName; }
	float GetDissolveBandHalfHeight() const { return DissolveBandHalfHeight; }
	void SetDissolveBandHalfHeight(float InHalfHeight) { DissolveBandHalfHeight = InHalfHeight; }

private:
	// 에미터 원점 기준 오프셋
	UPROPERTY(EditAnywhere)
	FVectorDistribution StartLocation;

	UPROPERTY(EditAnywhere)
	bool bSampleSkeletalMesh = false;

	UPROPERTY(EditAnywhere)
	bool bFilterByDissolveHeight = false;

	UPROPERTY(EditAnywhere)
	FString DissolveHeightParamName = "DissolveHeight";

	UPROPERTY(EditAnywhere)
	float DissolveBandHalfHeight = 0.25f;
};

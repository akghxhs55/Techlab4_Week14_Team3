#pragma once

#include "Core/Types.h"
#include "Particles/ParticleModule.h"
#include "Particles/Distributions.h"
#include "Particles/Module/ParticleModuleBeamSource.h"
#include "ParticleModuleBeamTarget.generated.h"

UCLASS()
class UParticleModuleBeamTarget : public UParticleModule
{
	GENERATED_BODY()
public:
	UParticleModuleBeamTarget();

	EModuleType GetModuleType() const override { return EPMT_Beam; }
	void BakeDistributions() override;

	EBeamEndpointMethod GetTargetMethod() const { return TargetMethod; }
	const FVectorDistribution& GetTarget() const { return Target; }
	bool ShouldUseTargetTangent() const { return bUseTargetTangent; }
	const FVectorDistribution& GetTargetTangent() const { return TargetTangent; }
	const FFloatDistribution& GetTargetStrength() const { return TargetStrength; }
	const FString& GetTargetParameterName() const { return TargetName; }

private:
	UPROPERTY(EditAnywhere)
	EBeamEndpointMethod TargetMethod = EBeamEndpointMethod::UserSet;

	UPROPERTY(EditAnywhere)
	FVectorDistribution Target;

	UPROPERTY(EditAnywhere)
	bool bUseTargetTangent = false;

	UPROPERTY(EditAnywhere)
	FVectorDistribution TargetTangent;

	UPROPERTY(EditAnywhere)
	FFloatDistribution TargetStrength;

	UPROPERTY(EditAnywhere)
	FString TargetName;
};

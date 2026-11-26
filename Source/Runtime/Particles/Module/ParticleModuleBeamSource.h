#pragma once

#include "Core/Types.h"
#include "Particles/ParticleModule.h"
#include "Particles/Distributions.h"
#include "ParticleModuleBeamSource.generated.h"

UENUM()
enum class EBeamEndpointMethod : uint8
{
	Emitter,
	UserSet,
	Parameter,
	Actor,
};

UCLASS()
class UParticleModuleBeamSource : public UParticleModule
{
	GENERATED_BODY()
public:
	UParticleModuleBeamSource();

	EModuleType GetModuleType() const override { return EPMT_Beam; }
	void BakeDistributions() override;

	EBeamEndpointMethod GetSourceMethod() const { return SourceMethod; }
	const FVectorDistribution& GetSource() const { return Source; }
	bool ShouldUseSourceTangent() const { return bUseSourceTangent; }
	const FVectorDistribution& GetSourceTangent() const { return SourceTangent; }
	const FFloatDistribution& GetSourceStrength() const { return SourceStrength; }
	const FString& GetSourceParameterName() const { return SourceName; }

private:
	UPROPERTY(EditAnywhere)
	EBeamEndpointMethod SourceMethod = EBeamEndpointMethod::Emitter;

	UPROPERTY(EditAnywhere)
	FVectorDistribution Source;

	UPROPERTY(EditAnywhere)
	bool bUseSourceTangent = false;

	UPROPERTY(EditAnywhere)
	FVectorDistribution SourceTangent;

	UPROPERTY(EditAnywhere)
	FFloatDistribution SourceStrength;

	UPROPERTY(EditAnywhere)
	FString SourceName;
};

#include "EnginePCH.h"
#include "ParticleModuleLocation.h"
#include "Particles/ParticleEmitterInstance.h"
#include "Components/ParticleSystemComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Render/SkeletalMeshObject.h"
#include <algorithm>
#include <cmath>

UParticleModuleLocation::UParticleModuleLocation()
{
	bSpawnModule = true;

	StartLocation.Mode = EDistributionMode::Uniform;
	StartLocation.MinValue = FVector(-20.0f, -20.0f, 0.0f);
	StartLocation.MaxValue = FVector(20.0f, 20.0f, 0.0f);

	// Build() 전에도 기본값이 동작하도록
	BakeDistributions();
}

void UParticleModuleLocation::BakeDistributions()
{
	StartLocation.Bake();
}

void UParticleModuleLocation::Spawn(FParticleEmitterInstance& Owner, int32 Offset, float SpawnTime, FBaseParticle& Particle)
{
	const FVector LocalOffset = StartLocation.GetVectorValue(Owner.EmitterTime, Owner.RandomStream);
	if (bSampleSkeletalMesh && Owner.Component)
	{
		if (USkeletalMeshComponent* SkelComp = Owner.Component->GetAfterimageSource())
		{
			const FSkeletalMeshObject* MeshObj = SkelComp->GetMeshObject();
			const TArray<FVertex>* SkinnedVerts = MeshObj ? MeshObj->GetCPUSkinnedVertices() : nullptr;
			if (SkinnedVerts && !SkinnedVerts->IsEmpty())
			{
				const FMatrix SkelWorld = SkelComp->GetWorldMatrix();
				const int32 TotalVerts = SkinnedVerts->Num();

				if (bFilterByDissolveHeight && !DissolveHeightParamName.empty())
				{
					float TargetZ = 0.0f;
					if (Owner.Component->GetFloatParameterValue(DissolveHeightParamName, TargetZ))
					{
						const float HalfBand = (std::max)(0.02f, DissolveBandHalfHeight);
						constexpr int32 MaxTries = 32;
						for (int32 TryIndex = 0; TryIndex < MaxTries; ++TryIndex)
						{
							const int32 RandIdx = static_cast<int32>(Owner.RandomStream.GetFraction() * static_cast<float>(TotalVerts - 1));
							const FVector WorldPos = SkelWorld.TransformPosition((*SkinnedVerts)[RandIdx].Position);
							if (std::abs(WorldPos.Z - TargetZ) <= HalfBand)
							{
								Particle.Location = Owner.WorldToSimulation(WorldPos) + Owner.EmitterToSimulationVector(LocalOffset);
								return;
							}
						}
					}
				}

				const int32 RandIdx = static_cast<int32>(Owner.RandomStream.GetFraction() * static_cast<float>(TotalVerts - 1));
				const FVector WorldPos = SkelWorld.TransformPosition((*SkinnedVerts)[RandIdx].Position);
				Particle.Location = Owner.WorldToSimulation(WorldPos) + Owner.EmitterToSimulationVector(LocalOffset);
				return;
			}
		}
	}
	Particle.Location += Owner.EmitterToSimulationVector(LocalOffset);
}

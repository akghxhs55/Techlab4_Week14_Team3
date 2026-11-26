#include "EnginePCH.h"
#include "ParticleAfterimageEmitterInstance.h"
#include "Components/ParticleSystemComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Render/SkeletalMeshObject.h"
#include "Render/SkeletalMesh.h"
#include "Particles/ParticleHelper.h"
#include "Particles/ParticleLODLevel.h"
#include "Particles/Module/ParticleModuleRequired.h"

bool FParticleAfterimageEmitterInstance::Resize(int32 NewMaxActiveParticles)
{
	if (!FParticleEmitterInstance::Resize(NewMaxActiveParticles))
	{
		return false;
	}
	if (Snapshots.Num() != MaxActiveParticles)
	{
		Snapshots.SetNum(MaxActiveParticles);
	}
	return true;
}

void FParticleAfterimageEmitterInstance::KillParticles()
{
	const int32 PrevActive = ActiveParticles;
	FParticleEmitterInstance::KillParticles();
	for (int32 i = ActiveParticles; i < PrevActive; ++i)
	{
		const uint16 Slot = ParticleIndices[i];
		if (Snapshots.IsValidIndex(Slot))
		{
			Snapshots[Slot].reset();
		}
	}
}

void FParticleAfterimageEmitterInstance::KillAllParticles()
{
	FParticleEmitterInstance::KillAllParticles();
	for (TSharedPtr<FAfterimageSnapshot>& Entry : Snapshots)
	{
		Entry.reset();
	}
}

void FParticleAfterimageEmitterInstance::PostSpawn(FBaseParticle* Particle, float SpawnTime)
{
	FParticleEmitterInstance::PostSpawn(Particle, SpawnTime);
	if (!Component || !Particle)
	{
		return;
	}

	const int32 SlotIndex = static_cast<int32>((reinterpret_cast<const uint8*>(Particle) - ParticleData) / ParticleStride);
	if (!Snapshots.IsValidIndex(SlotIndex))
	{
		return;
	}

	USkeletalMeshComponent* SkelComp = Component->GetAfterimageSource();
	if (!SkelComp || !SkelComp->GetSkeletalMesh() || !SkelComp->GetMeshObject())
	{
		Snapshots[SlotIndex].reset();
		return;
	}

	const TArray<FMatrix>& Matrices = SkelComp->GetMeshObject()->GetRefToLocalMatrices();
	if (Matrices.IsEmpty())
	{
		Snapshots[SlotIndex].reset();
		return;
	}

	TSharedPtr<FAfterimageSnapshot> Snapshot = MakeShared<FAfterimageSnapshot>();
	Snapshot->Mesh = SkelComp->GetSkeletalMesh();
	Snapshot->World = SkelComp->GetWorldMatrix();
	Snapshot->WorldBounds = SkelComp->CalcBounds();
	Snapshot->SkinningMatrices = Matrices;
	Snapshots[SlotIndex] = std::move(Snapshot);
}

void FParticleAfterimageEmitterInstance::UpdateBoundingBox(float DeltaTime)
{
	FParticleEmitterInstance::UpdateBoundingBox(DeltaTime);
	for (int32 i = 0; i < ActiveParticles; ++i)
	{
		const uint16 Slot = ParticleIndices[i];
		if (Snapshots.IsValidIndex(Slot) && Snapshots[Slot])
		{
			BoundingBox.Expand(Snapshots[Slot]->WorldBounds);
		}
	}
}

void FParticleAfterimageEmitterInstance::UpdateDynamicData(TUniquePtr<FDynamicEmitterDataBase>& InOutData) const
{
	if (!InOutData || InOutData->GetType() != EDynamicEmitterType::Afterimage)
	{
		InOutData = MakeUnique<FDynamicAfterimageEmitterData>();
	}

	FDynamicAfterimageEmitterData& Data = static_cast<FDynamicAfterimageEmitterData&>(*InOutData);
	FillReplayData(Data.Source);
	Data.Particles.Reset();
	for (int32 i = 0; i < ActiveParticles; ++i)
	{
		const uint16 Slot = ParticleIndices[i];
		if (!Snapshots.IsValidIndex(Slot) || !Snapshots[Slot])
		{
			continue;
		}
		const FBaseParticle* Particle = GetParticle(Slot);
		if (!Particle)
		{
			continue;
		}
		const float LifeAlpha = std::clamp(1.0f - Particle->RelativeTime, 0.0f, 1.0f);
		Data.Particles.Add({ Snapshots[Slot], LifeAlpha * Particle->Color.A });
	}
}

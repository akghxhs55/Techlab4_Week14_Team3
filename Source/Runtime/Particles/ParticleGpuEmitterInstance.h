#pragma once

#include "Particles/ParticleEmitterInstance.h"
#include "Render/MeshBatch.h"
#include "Render/ParticleCurveTexture.h"

class UStaticMesh;

struct FParticleGpuEmitterInstance : public FParticleEmitterInstance
{
	FParticleGpuEmitterInstance() = default;
	~FParticleGpuEmitterInstance() override;

	void Init(UParticleEmitter* InTemplate, UParticleSystemComponent* InComponent) override;
	void Tick(float DeltaTime) override;
	void UpdateDynamicData(TUniquePtr<FDynamicEmitterDataBase>& InOutData) const override;
	void SetCurrentLODIndex(int32 InLODIndex) override;

protected:
	void SpawnParticles(int32 Count, float StartTime, float Increment, const FVector& InitialLocation, const FVector& ExtraVelocity);
	void UpdateBoundingBox(float DeltaTime) override;

private:
	void RebuildGpuDistributions();
	void ReleaseCurveHandles();

	TSharedPtr<FGpuParticleEmitterResources> GpuResources;
	mutable TArray<uint8> PendingSpawnBytes;
	mutable uint32 PendingSpawnCount = 0;
	mutable uint32 WriteCursor = 0;
	mutable bool bResetRequested = true;
	mutable float PendingDeltaTime = 0.0f;

	uint32 MaxGpuParticles = 2048;
	UStaticMesh* MeshAsset = nullptr;
	FString ColorParameterName;

	FGpuDistributionDesc SizeScaleDesc{};
	FGpuDistributionDesc ColorOverLifeDesc{};
	FGpuDistributionDesc SubUVDesc{};
	FGpuDistributionDesc VelocityScaleDesc{};
	FGpuDistributionDesc RotationRateDesc{};

	FParticleCurveAtlasHandle SizeHandle{};
	FParticleCurveAtlasHandle ColorHandle{};
	FParticleCurveAtlasHandle SubUVHandle{};
	FParticleCurveAtlasHandle VelocityHandle{};
	FParticleCurveAtlasHandle RotationHandle{};

	bool bHasCollision = false;
	float CollisionRestitution = 0.5f;
	float CollisionFriction = 0.2f;
	uint32 CollisionMaxCount = 10;
	float CollisionRadius = 1.0f;
	float DepthThickness = 1.0f;
	FVector GpuAcceleration = FVector::ZeroVector;
};

#include "EnginePCH.h"
#include "ParticleGpuEmitterInstance.h"
#include "Particles/ParticleEmitter.h"
#include "Particles/ParticleLODLevel.h"
#include "Particles/ParticleModule.h"
#include "Particles/ParticleHelper.h"
#include "Particles/Module/ParticleModuleRequired.h"
#include "Particles/Module/ParticleModuleSpawn.h"
#include "Particles/Module/ParticleModuleSpawnPerUnit.h"
#include "Particles/Module/ParticleModuleColorOverLife.h"
#include "Particles/Module/ParticleModuleSizeMultiplyLife.h"
#include "Particles/Module/ParticleModuleVelocityOverLife.h"
#include "Particles/Module/ParticleModuleRotationRateOverLife.h"
#include "Particles/Module/ParticleModuleSubUV.h"
#include "Particles/Module/ParticleModuleAcceleration.h"
#include "Particles/Module/ParticleModuleCollision.h"
#include "Particles/Module/ParticleModuleTypeDataGpuSprite.h"
#include "Particles/Module/ParticleModuleTypeDataGpuMesh.h"
#include "Components/ParticleSystemComponent.h"
#include "Engine/ParticleSystemRender.h"
#include "Render/Mesh.h"
#include <algorithm>
#include <cstring>

namespace
{
	FGpuDistributionDesc UploadVectorCurveToAtlas(const FVectorDistribution& Dist, FParticleCurveAtlasHandle& OutHandle)
	{
		FGpuDistributionDesc Desc{};
		TArray<FVector4> Samples;
		const uint32 SampleCount = FParticleCurveTexture::MaxCurveSampleWidth;
		Samples.SetNum(SampleCount);
		FRandomStream DummyStream(1);
		for (uint32 i = 0; i < SampleCount; ++i)
		{
			const float T = SampleCount > 1 ? static_cast<float>(i) / static_cast<float>(SampleCount - 1) : 0.0f;
			const FVector Val = const_cast<FVectorDistribution&>(Dist).GetVectorValue(T, DummyStream);
			Samples[i] = FVector4(Val.X, Val.Y, Val.Z, 1.0f);
		}
		OutHandle = FParticleCurveTexture::Get().AllocateCurve(Samples);
		if (OutHandle.IsValid())
		{
			Desc.AtlasUVAndMode = FVector4(OutHandle.UStart, OutHandle.UScale, OutHandle.VCoord, 2.0f);
			Desc.ValueScale = FVector4(1.0f, 1.0f, 1.0f, 1.0f);
			Desc.ValueBias = FVector4(0.0f, 0.0f, 0.0f, 0.0f);
		}
		return Desc;
	}

	FGpuDistributionDesc UploadColorCurveToAtlas(const FVectorDistribution& RGBDist, const FFloatDistribution& AlphaDist, FParticleCurveAtlasHandle& OutHandle)
	{
		FGpuDistributionDesc Desc{};
		TArray<FVector4> Samples;
		const uint32 SampleCount = FParticleCurveTexture::MaxCurveSampleWidth;
		Samples.SetNum(SampleCount);
		FRandomStream DummyStream(1);
		for (uint32 i = 0; i < SampleCount; ++i)
		{
			const float T = SampleCount > 1 ? static_cast<float>(i) / static_cast<float>(SampleCount - 1) : 0.0f;
			const FVector C = const_cast<FVectorDistribution&>(RGBDist).GetVectorValue(T, DummyStream);
			const float A = const_cast<FFloatDistribution&>(AlphaDist).GetFloatValue(T, DummyStream);
			Samples[i] = FVector4(C.X, C.Y, C.Z, A);
		}
		OutHandle = FParticleCurveTexture::Get().AllocateCurve(Samples);
		if (OutHandle.IsValid())
		{
			Desc.AtlasUVAndMode = FVector4(OutHandle.UStart, OutHandle.UScale, OutHandle.VCoord, 2.0f);
			Desc.ValueScale = FVector4(1.0f, 1.0f, 1.0f, 1.0f);
			Desc.ValueBias = FVector4(0.0f, 0.0f, 0.0f, 0.0f);
		}
		return Desc;
	}

	FGpuDistributionDesc UploadFloatCurveToAtlas(const FFloatDistribution& Dist, FParticleCurveAtlasHandle& OutHandle)
	{
		FGpuDistributionDesc Desc{};
		TArray<FVector4> Samples;
		const uint32 SampleCount = FParticleCurveTexture::MaxCurveSampleWidth;
		Samples.SetNum(SampleCount);
		FRandomStream DummyStream(1);
		for (uint32 i = 0; i < SampleCount; ++i)
		{
			const float T = SampleCount > 1 ? static_cast<float>(i) / static_cast<float>(SampleCount - 1) : 0.0f;
			const float V = const_cast<FFloatDistribution&>(Dist).GetFloatValue(T, DummyStream);
			Samples[i] = FVector4(V, V, V, V);
		}
		OutHandle = FParticleCurveTexture::Get().AllocateCurve(Samples);
		if (OutHandle.IsValid())
		{
			Desc.AtlasUVAndMode = FVector4(OutHandle.UStart, OutHandle.UScale, OutHandle.VCoord, 2.0f);
			Desc.ValueScale = FVector4(1.0f, 1.0f, 1.0f, 1.0f);
			Desc.ValueBias = FVector4(0.0f, 0.0f, 0.0f, 0.0f);
		}
		return Desc;
	}
}

FParticleGpuEmitterInstance::~FParticleGpuEmitterInstance()
{
	ReleaseCurveHandles();
}

void FParticleGpuEmitterInstance::ReleaseCurveHandles()
{
	if (!FParticleCurveTexture::IsInitialized())
	{
		SizeHandle.Invalidate();
		ColorHandle.Invalidate();
		SubUVHandle.Invalidate();
		VelocityHandle.Invalidate();
		RotationHandle.Invalidate();
		return;
	}
	FParticleCurveTexture& Atlas = FParticleCurveTexture::Get();
	Atlas.FreeCurve(SizeHandle);
	Atlas.FreeCurve(ColorHandle);
	Atlas.FreeCurve(SubUVHandle);
	Atlas.FreeCurve(VelocityHandle);
	Atlas.FreeCurve(RotationHandle);
}

void FParticleGpuEmitterInstance::Init(UParticleEmitter* InTemplate, UParticleSystemComponent* InComponent)
{
	SpriteTemplate = InTemplate;
	Component = InComponent;
	CurrentLODIndex = 0;
	CurrentLODLevel = InTemplate ? InTemplate->GetLODLevel(0) : nullptr;
	if (!GpuResources)
	{
		GpuResources = MakeShared<FGpuParticleEmitterResources>();
	}
	bResetRequested = true;
	PendingSpawnBytes.Reset();
	PendingSpawnCount = 0;
	WriteCursor = 0;
	PendingDeltaTime = 0.0f;
	RandomStream.Initialize(static_cast<int32>(reinterpret_cast<uintptr_t>(this) & 0x7fffffff));
	RebuildGpuDistributions();
}

void FParticleGpuEmitterInstance::SetCurrentLODIndex(int32 InLODIndex)
{
	FParticleEmitterInstance::SetCurrentLODIndex(InLODIndex);
	RebuildGpuDistributions();
}

void FParticleGpuEmitterInstance::RebuildGpuDistributions()
{
	ReleaseCurveHandles();
	SizeScaleDesc = {};
	ColorOverLifeDesc = {};
	SubUVDesc = {};
	VelocityScaleDesc = {};
	RotationRateDesc = {};
	bHasCollision = false;
	GpuAcceleration = FVector::ZeroVector;
	MeshAsset = nullptr;
	MaxGpuParticles = 2048;

	if (!CurrentLODLevel)
	{
		return;
	}

	if (const UParticleModuleTypeDataGpuMesh* GpuMeshTD = Cast<UParticleModuleTypeDataGpuMesh>(CurrentLODLevel->GetTypeDataModule()))
	{
		MeshAsset = GpuMeshTD->GetMesh();
		MaxGpuParticles = static_cast<uint32>((std::max)(1, GpuMeshTD->GetMaxGpuParticles()));
	}
	else if (const UParticleModuleTypeDataGpuSprite* GpuSpriteTD = Cast<UParticleModuleTypeDataGpuSprite>(CurrentLODLevel->GetTypeDataModule()))
	{
		MaxGpuParticles = static_cast<uint32>((std::max)(1, GpuSpriteTD->GetMaxGpuParticles()));
	}
	if (const UParticleModuleRequired* Required = CurrentLODLevel->GetRequiredModule())
	{
		if (Required->GetMaxActiveParticles() > 0)
		{
			MaxGpuParticles = static_cast<uint32>(Required->GetMaxActiveParticles());
		}
	}

	for (UParticleModule* Module : CurrentLODLevel->GetModules())
	{
		if (!Module || !Module->IsEnabled())
		{
			continue;
		}
		if (const UParticleModuleSizeMultiplyLife* SizeMod = Cast<UParticleModuleSizeMultiplyLife>(Module))
		{
			SizeScaleDesc = UploadVectorCurveToAtlas(SizeMod->GetLifeMultiplier(), SizeHandle);
		}
		else if (const UParticleModuleColorOverLife* ColorMod = Cast<UParticleModuleColorOverLife>(Module))
		{
			ColorOverLifeDesc = UploadColorCurveToAtlas(ColorMod->GetColorOverLife(), ColorMod->GetAlphaOverLife(), ColorHandle);
		}
		else if (const UParticleModuleVelocityOverLife* VelMod = Cast<UParticleModuleVelocityOverLife>(Module))
		{
			VelocityScaleDesc = UploadVectorCurveToAtlas(const_cast<UParticleModuleVelocityOverLife*>(VelMod)->GetVelOverLife(), VelocityHandle);
		}
		else if (const UParticleModuleRotationRateOverLife* RotMod = Cast<UParticleModuleRotationRateOverLife>(Module))
		{
			RotationRateDesc = UploadFloatCurveToAtlas(const_cast<UParticleModuleRotationRateOverLife*>(RotMod)->GetLifeMultiplier(), RotationHandle);
		}
		else if (const UParticleModuleAcceleration* AccelMod = Cast<UParticleModuleAcceleration>(Module))
		{
			FRandomStream Stream(1);
			GpuAcceleration = const_cast<UParticleModuleAcceleration*>(AccelMod)->GetAcceleration().GetVectorValue(0.0f, Stream);
		}
		else if (const UParticleModuleCollision* ColMod = Cast<UParticleModuleCollision>(Module))
		{
			bHasCollision = true;
			FRandomStream Stream(1);
			CollisionRestitution = const_cast<FFloatDistribution&>(ColMod->GetRestitution()).GetFloatValue(0.0f, Stream);
			CollisionFriction = const_cast<FFloatDistribution&>(ColMod->GetFriction()).GetFloatValue(0.0f, Stream);
			CollisionMaxCount = static_cast<uint32>((std::max)(0, ColMod->GetMaxCollisions()));
			CollisionRadius = ColMod->GetRadiusScale();
		}
	}
}

void FParticleGpuEmitterInstance::SpawnParticles(int32 Count, float StartTime, float Increment,
	const FVector& InitialLocation, const FVector& ExtraVelocity)
{
	if (Count <= 0 || !CurrentLODLevel || !SpriteTemplate || MaxGpuParticles == 0)
	{
		return;
	}

	const int32 ParticleBytes = SpriteTemplate->GetParticleSize() > static_cast<int32>(sizeof(FBaseParticle))
		? SpriteTemplate->GetParticleSize()
		: static_cast<int32>(sizeof(FBaseParticle));
	TArray<uint8> ScratchMemory;
	ScratchMemory.SetNum(ParticleBytes);

	for (int32 i = 0; i < Count; ++i)
	{
		std::memset(ScratchMemory.GetData(), 0, ScratchMemory.Num());
		FBaseParticle* Particle = reinterpret_cast<FBaseParticle*>(ScratchMemory.GetData());
		const float SpawnTime = StartTime - Increment * static_cast<float>(i);
		PreSpawn(Particle, InitialLocation);

		for (UParticleModule* Module : CurrentLODLevel->GetSpawnModules())
		{
			if (!Module || !Module->IsEnabled())
			{
				continue;
			}
			Module->Spawn(*this, SpriteTemplate->GetModuleOffset(Module), SpawnTime, *Particle);
		}
		Particle->Velocity += ExtraVelocity;
		Particle->BaseVelocity += ExtraVelocity;
		PostSpawn(Particle, SpawnTime);

		FGpuParticleState State{};
		State.Position = Particle->Location;
		State.RelativeTime = Particle->RelativeTime;
		State.Velocity = Particle->Velocity;
		State.Lifetime = Particle->OneOverMaxLifetime > 0.0f ? (1.0f / Particle->OneOverMaxLifetime) : 1.0f;
		State.BaseSize = Particle->BaseSize;
		State.Rotation = Particle->Rotation;
		State.Color = FVector4(Particle->Color.R, Particle->Color.G, Particle->Color.B, Particle->Color.A);
		State.Size = Particle->Size;
		State.RotationRate = Particle->RotationRate;
		State.BaseColor = FVector4(Particle->BaseColor.R, Particle->BaseColor.G, Particle->BaseColor.B, Particle->BaseColor.A);
		State.BaseVelocity = Particle->BaseVelocity;
		State.BaseRotationRate = Particle->BaseRotationRate;
		State.bAlive = 1;
		State.MeshRotation = FVector4(0.0f, 0.0f, 0.0f, 1.0f);

		const int32 Offset = PendingSpawnBytes.Num();
		PendingSpawnBytes.SetNum(Offset + static_cast<int32>(sizeof(FGpuParticleState)));
		std::memcpy(PendingSpawnBytes.GetData() + Offset, &State, sizeof(FGpuParticleState));
		++PendingSpawnCount;
	}
}

void FParticleGpuEmitterInstance::Tick(float DeltaTime)
{
	if (!CurrentLODLevel || !CurrentLODLevel->IsEnabled())
	{
		return;
	}

	PendingDeltaTime += DeltaTime;
	UpdateEmitterTime(DeltaTime);

	if (!bSpawningEnabled || EmitterTime < 0.0f || HasCompletedLoops())
	{
		UpdateBoundingBox(DeltaTime);
		return;
	}

	const UParticleModuleSpawn* SpawnModule = CurrentLODLevel->GetSpawnModule();
	if (!SpawnModule || !SpawnModule->IsEnabled())
	{
		UpdateBoundingBox(DeltaTime);
		return;
	}

	const float Rate = SpawnModule->GetSpawnRate(EmitterTime, RandomStream);
	const float OldLeftover = SpawnFraction;
	const float NewLeftover = OldLeftover + DeltaTime * Rate;
	const int32 Number = FMath::FloorToInt(NewLeftover);
	const float Increment = Rate > 0.0f ? 1.0f / Rate : 0.0f;
	const float StartTime = DeltaTime + OldLeftover * Increment - Increment;
	SpawnFraction = NewLeftover - Number;

	const int32 BurstNumber = ComputeBurstCount();
	const int32 TotalToSpawn = Number + BurstNumber;
	if (TotalToSpawn > 0)
	{
		SpawnParticles(TotalToSpawn, StartTime, Increment, GetEmitterOrigin(), FVector::ZeroVector);
	}
	UpdateBoundingBox(DeltaTime);
}

void FParticleGpuEmitterInstance::UpdateBoundingBox(float DeltaTime)
{
	const FVector Center = Component ? Component->GetWorldLocation() : FVector::ZeroVector;
	const FVector Extent(500.0f, 500.0f, 500.0f);
	BoundingBox = FBox(Center - Extent, Center + Extent);
}

void FParticleGpuEmitterInstance::UpdateDynamicData(TUniquePtr<FDynamicEmitterDataBase>& InOutData) const
{
	if (!InOutData || InOutData->GetType() != EDynamicEmitterType::Gpu)
	{
		InOutData = MakeUnique<FDynamicGpuEmitterData>();
	}

	FDynamicGpuEmitterData& Data = static_cast<FDynamicGpuEmitterData&>(*InOutData);
	Data.GpuResources = GpuResources;
	Data.MeshAsset = MeshAsset;
	Data.MaxParticles = MaxGpuParticles;
	Data.bUseLocalSpace = IsLocalSpace();
	Data.LocalToWorld = Component ? Component->GetWorldMatrix() : FMatrix::Identity;
	Data.WorldLocation = Component ? Component->GetWorldLocation() : FVector::ZeroVector;
	Data.WorldScale = GetComponentScale();
	Data.bResetRequested = bResetRequested;

	if (const UParticleModuleRequired* Required = CurrentLODLevel ? CurrentLODLevel->GetRequiredModule() : nullptr)
	{
		Data.Source.Material = Required->GetMaterial();
		Data.AtlasParams = FVector4(
			static_cast<float>(Required->GetSubImagesHorizontal()),
			static_cast<float>(Required->GetSubImagesVertical()),
			1.0f,
			Required->UsesSubUV() ? 1.0f : 0.0f);
	}

	FGpuParticleSimParams Params{};
	Params.DeltaTime = PendingDeltaTime;
	Params.MaxParticles = MaxGpuParticles;
	Params.SpawnCount = PendingSpawnCount;
	Params.WriteCursor = WriteCursor;
	Params.SizeScale = SizeScaleDesc;
	Params.ColorOverLife = ColorOverLifeDesc;
	Params.SubUV = SubUVDesc;
	Params.VelocityScale = VelocityScaleDesc;
	Params.RotationRateScale = RotationRateDesc;
	Params.Acceleration = GpuAcceleration;
	Params.HasCollision = bHasCollision ? 1u : 0u;
	Params.CollisionRestitution = CollisionRestitution;
	Params.CollisionFriction = CollisionFriction;
	Params.CollisionMaxCount = CollisionMaxCount;
	Params.CollisionRadius = CollisionRadius;
	Params.DepthThickness = DepthThickness;
	if (const UParticleModuleRequired* Required = CurrentLODLevel ? CurrentLODLevel->GetRequiredModule() : nullptr)
	{
		Params.SubUVCols = static_cast<float>(Required->GetSubImagesHorizontal());
		Params.SubUVRows = static_cast<float>(Required->GetSubImagesVertical());
		Params.SubUVPlayRate = 1.0f;
		Params.SubUVLinear = Required->UsesSubUV() ? 1u : 0u;
	}
	Data.SimParams = Params;
	Data.PendingSpawnBytes = std::move(PendingSpawnBytes);

	if (MaxGpuParticles > 0 && PendingSpawnCount > 0)
	{
		WriteCursor = (WriteCursor + PendingSpawnCount) % MaxGpuParticles;
	}
	PendingSpawnBytes.Reset();
	PendingSpawnCount = 0;
	PendingDeltaTime = 0.0f;
	bResetRequested = false;
}

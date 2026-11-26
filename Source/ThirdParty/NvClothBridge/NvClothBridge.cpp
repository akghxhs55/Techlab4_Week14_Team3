#include "ClothBridge.h"

#include <NvCloth/Callbacks.h>
#include <NvCloth/Cloth.h>
#include <NvCloth/Fabric.h>
#include <NvCloth/Factory.h>
#include <NvCloth/PhaseConfig.h>
#include <NvCloth/Solver.h>
#include <NvClothExt/ClothFabricCooker.h>
#include <NvClothExt/ClothMeshDesc.h>
#include <foundation/PxAllocatorCallback.h>
#include <foundation/PxErrorCallback.h>
#include <foundation/PxQuat.h>
#include <foundation/PxVec3.h>
#include <foundation/PxVec4.h>

#include <cstdlib>
#include <cstring>
#include <vector>

namespace
{
	FClothLogFn GLogCallback = nullptr;
	bool GBridgeInitialized = false;

	class FBridgeAllocator final : public physx::PxAllocatorCallback
	{
	public:
		void* allocate(size_t Size, const char*, const char*, int) override
		{
			return _aligned_malloc(Size, 16);
		}

		void deallocate(void* Ptr) override
		{
			if (Ptr)
			{
				_aligned_free(Ptr);
			}
		}
	};

	class FBridgeErrorCallback final : public physx::PxErrorCallback
	{
	public:
		void reportError(physx::PxErrorCode::Enum Code, const char* Message, const char*, int) override
		{
			if (!GLogCallback || !Message)
			{
				return;
			}
			int32_t Level = 0;
			if (Code == physx::PxErrorCode::eDEBUG_WARNING || Code == physx::PxErrorCode::ePERF_WARNING)
			{
				Level = 1;
			}
			else if (Code != physx::PxErrorCode::eDEBUG_INFO)
			{
				Level = 2;
			}
			GLogCallback(Level, Message);
		}
	};

	class FBridgeAssertHandler final : public physx::PxAssertHandler
	{
	public:
		void operator()(const char* Exp, const char*, int, bool& Ignore) override
		{
			Ignore = true;
			if (GLogCallback && Exp)
			{
				GLogCallback(2, Exp);
			}
		}
	};

	FBridgeAllocator GAllocator;
	FBridgeErrorCallback GErrorCallback;
	FBridgeAssertHandler GAssertHandler;

	struct FFabricBundle
	{
		nv::cloth::Fabric* Fabric = nullptr;
		std::vector<int32_t> PhaseTypes;
	};
}

bool ClothBridge_Initialize(FClothLogFn LogCallback)
{
	GLogCallback = LogCallback;
	if (GBridgeInitialized)
	{
		return true;
	}

	nv::cloth::InitializeNvCloth(&GAllocator, &GErrorCallback, &GAssertHandler, nullptr);
	GBridgeInitialized = true;
	return true;
}

void ClothBridge_Shutdown()
{
	GLogCallback = nullptr;
	GBridgeInitialized = false;
}

FClothFactoryHandle ClothBridge_CreateFactoryCPU()
{
	return NvClothCreateFactoryCPU();
}

void ClothBridge_DestroyFactory(FClothFactoryHandle Factory)
{
	if (Factory)
	{
		NvClothDestroyFactory(static_cast<nv::cloth::Factory*>(Factory));
	}
}

FClothFabricHandle ClothBridge_CookFabric(FClothFactoryHandle Factory, const FClothBridgeMeshDesc* MeshDesc)
{
	if (!Factory || !MeshDesc || !MeshDesc->Positions || !MeshDesc->Indices || MeshDesc->VertexCount == 0 || MeshDesc->TriangleCount == 0)
	{
		return nullptr;
	}

	nv::cloth::ClothMeshDesc Desc;
	Desc.setToDefault();
	Desc.points.data = MeshDesc->Positions;
	Desc.points.count = MeshDesc->VertexCount;
	Desc.points.stride = MeshDesc->PositionStride;

	if (MeshDesc->InvMasses)
	{
		Desc.invMasses.data = MeshDesc->InvMasses;
		Desc.invMasses.count = MeshDesc->VertexCount;
		Desc.invMasses.stride = MeshDesc->InvMassStride;
	}

	Desc.triangles.data = MeshDesc->Indices;
	Desc.triangles.count = MeshDesc->TriangleCount;
	Desc.triangles.stride = sizeof(uint32_t) * 3;
	Desc.flags = 0;

	nv::cloth::Vector<int32_t>::Type RawPhaseTypes;
	const physx::PxVec3 Gravity(MeshDesc->CookGravity.X, MeshDesc->CookGravity.Y, MeshDesc->CookGravity.Z);
	nv::cloth::Fabric* Fabric = NvClothCookFabricFromMesh(
		static_cast<nv::cloth::Factory*>(Factory),
		Desc,
		Gravity,
		&RawPhaseTypes,
		MeshDesc->bUseGeodesicTether);

	if (!Fabric)
	{
		return nullptr;
	}

	FFabricBundle* Bundle = new FFabricBundle();
	Bundle->Fabric = Fabric;
	Bundle->PhaseTypes.assign(RawPhaseTypes.begin(), RawPhaseTypes.end());
	return Bundle;
}

void ClothBridge_DestroyFabric(FClothFabricHandle Fabric)
{
	if (!Fabric)
	{
		return;
	}
	FFabricBundle* Bundle = static_cast<FFabricBundle*>(Fabric);
	if (Bundle->Fabric)
	{
		Bundle->Fabric->decRefCount();
	}
	delete Bundle;
}

FClothSolverHandle ClothBridge_CreateSolver(FClothFactoryHandle Factory)
{
	if (!Factory)
	{
		return nullptr;
	}
	return static_cast<nv::cloth::Factory*>(Factory)->createSolver();
}

void ClothBridge_DestroySolver(FClothSolverHandle Solver)
{
	if (Solver)
	{
		delete static_cast<nv::cloth::Solver*>(Solver);
	}
}

void ClothBridge_AddClothToSolver(FClothSolverHandle Solver, FClothInstanceHandle Cloth)
{
	if (Solver && Cloth)
	{
		static_cast<nv::cloth::Solver*>(Solver)->addCloth(static_cast<nv::cloth::Cloth*>(Cloth));
	}
}

void ClothBridge_RemoveClothFromSolver(FClothSolverHandle Solver, FClothInstanceHandle Cloth)
{
	if (Solver && Cloth)
	{
		static_cast<nv::cloth::Solver*>(Solver)->removeCloth(static_cast<nv::cloth::Cloth*>(Cloth));
	}
}

void ClothBridge_StepSolver(FClothSolverHandle Solver, float DeltaTime)
{
	if (!Solver || DeltaTime <= 0.0f)
	{
		return;
	}
	nv::cloth::Solver* NvSolver = static_cast<nv::cloth::Solver*>(Solver);
	if (!NvSolver->beginSimulation(DeltaTime))
	{
		return;
	}
	const int ChunkCount = NvSolver->getSimulationChunkCount();
	for (int ChunkIndex = 0; ChunkIndex < ChunkCount; ++ChunkIndex)
	{
		NvSolver->simulateChunk(ChunkIndex);
	}
	NvSolver->endSimulation();
}

FClothInstanceHandle ClothBridge_CreateCloth(FClothFactoryHandle Factory, FClothFabricHandle Fabric, const FClothBridgeVec4* InitialParticles, uint32_t ParticleCount)
{
	if (!Factory || !Fabric || !InitialParticles || ParticleCount == 0)
	{
		return nullptr;
	}
	FFabricBundle* Bundle = static_cast<FFabricBundle*>(Fabric);
	if (!Bundle->Fabric)
	{
		return nullptr;
	}

	std::vector<physx::PxVec4> PxParticles(ParticleCount);
	for (uint32_t i = 0; i < ParticleCount; ++i)
	{
		PxParticles[i] = physx::PxVec4(InitialParticles[i].X, InitialParticles[i].Y, InitialParticles[i].Z, InitialParticles[i].W);
	}

	nv::cloth::Range<const physx::PxVec4> ParticleRange(PxParticles.data(), PxParticles.data() + PxParticles.size());
	return static_cast<nv::cloth::Factory*>(Factory)->createCloth(ParticleRange, *Bundle->Fabric);
}

void ClothBridge_DestroyCloth(FClothInstanceHandle Cloth)
{
	if (Cloth)
	{
		delete static_cast<nv::cloth::Cloth*>(Cloth);
	}
}

void ClothBridge_ApplyParams(FClothInstanceHandle Cloth, FClothFabricHandle Fabric, const FClothBridgeParams* Params)
{
	if (!Cloth || !Params)
	{
		return;
	}
	nv::cloth::Cloth* NvCloth = static_cast<nv::cloth::Cloth*>(Cloth);
	NvCloth->setGravity(physx::PxVec3(Params->Gravity.X, Params->Gravity.Y, Params->Gravity.Z));
	NvCloth->setDamping(physx::PxVec3(Params->Damping.X, Params->Damping.Y, Params->Damping.Z));
	NvCloth->setLinearDrag(physx::PxVec3(Params->LinearDrag.X, Params->LinearDrag.Y, Params->LinearDrag.Z));
	NvCloth->setAngularDrag(physx::PxVec3(Params->AngularDrag.X, Params->AngularDrag.Y, Params->AngularDrag.Z));
	NvCloth->setLinearInertia(physx::PxVec3(Params->LinearInertia.X, Params->LinearInertia.Y, Params->LinearInertia.Z));
	NvCloth->setAngularInertia(physx::PxVec3(Params->AngularInertia.X, Params->AngularInertia.Y, Params->AngularInertia.Z));
	NvCloth->setCentrifugalInertia(physx::PxVec3(Params->CentrifugalInertia.X, Params->CentrifugalInertia.Y, Params->CentrifugalInertia.Z));
	NvCloth->setWindVelocity(physx::PxVec3(Params->WindVelocity.X, Params->WindVelocity.Y, Params->WindVelocity.Z));
	NvCloth->setDragCoefficient(Params->DragCoefficient);
	NvCloth->setLiftCoefficient(Params->LiftCoefficient);
	NvCloth->setFluidDensity(Params->FluidDensity);
	NvCloth->setSolverFrequency(Params->SolverFrequency);
	NvCloth->setStiffnessFrequency(Params->StiffnessFrequency);
	NvCloth->setTetherConstraintScale(Params->TetherScale);
	NvCloth->setTetherConstraintStiffness(Params->TetherStiffness);
	NvCloth->setFriction(Params->Friction);
	NvCloth->setMotionConstraintStiffness(Params->MotionConstraintStiffness);
	NvCloth->setSelfCollisionDistance(Params->SelfCollisionDistance);
	NvCloth->setSelfCollisionStiffness(Params->SelfCollisionStiffness);
	NvCloth->enableContinuousCollision(Params->bEnableContinuousCollision);

	if (Fabric)
	{
		FFabricBundle* Bundle = static_cast<FFabricBundle*>(Fabric);
		if (Bundle->Fabric && !Bundle->PhaseTypes.empty())
		{
			const uint32_t NumPhases = Bundle->Fabric->getNumPhases();
			std::vector<nv::cloth::PhaseConfig> Phases(NumPhases);
			for (uint32_t i = 0; i < NumPhases; ++i)
			{
				Phases[i] = nv::cloth::PhaseConfig(static_cast<uint16_t>(i));
				Phases[i].mCompressionLimit = Params->CompressionLimit;
				Phases[i].mStretchLimit = Params->StretchLimit;
				Phases[i].mStiffnessMultiplier = Params->StiffnessMultiplier;
				const int32_t PhaseType = (i < Bundle->PhaseTypes.size()) ? Bundle->PhaseTypes[i] : nv::cloth::ClothFabricPhaseType::eINVALID;
				switch (PhaseType)
				{
				case nv::cloth::ClothFabricPhaseType::eVERTICAL:
				case nv::cloth::ClothFabricPhaseType::eHORIZONTAL:
					Phases[i].mStiffness = Params->StretchStiffness;
					break;
				case nv::cloth::ClothFabricPhaseType::eBENDING:
					Phases[i].mStiffness = Params->BendStiffness;
					break;
				case nv::cloth::ClothFabricPhaseType::eSHEARING:
					Phases[i].mStiffness = Params->ShearStiffness;
					break;
				default:
					Phases[i].mStiffness = Params->StretchStiffness;
					break;
				}
			}
			NvCloth->setPhaseConfig(nv::cloth::Range<const nv::cloth::PhaseConfig>(Phases.data(), Phases.data() + Phases.size()));
		}
	}
}

void ClothBridge_SetTransform(FClothInstanceHandle Cloth, const FClothBridgeVec3* Translation, const FClothBridgeQuat* Rotation, bool bTeleport)
{
	if (!Cloth || !Translation || !Rotation)
	{
		return;
	}
	nv::cloth::Cloth* NvCloth = static_cast<nv::cloth::Cloth*>(Cloth);
	const physx::PxVec3 PxPos(Translation->X, Translation->Y, Translation->Z);
	const physx::PxQuat PxRot(Rotation->X, Rotation->Y, Rotation->Z, Rotation->W);
	if (bTeleport)
	{
		NvCloth->teleportToLocation(PxPos, PxRot);
	}
	else
	{
		NvCloth->setTranslation(PxPos);
		NvCloth->setRotation(PxRot);
	}
}

void ClothBridge_SetSpheresAndCapsules(FClothInstanceHandle Cloth, const FClothBridgeVec4* Spheres, uint32_t SphereCount, const uint32_t* CapsuleIndices, uint32_t CapsuleIndexCount)
{
	if (!Cloth)
	{
		return;
	}
	nv::cloth::Cloth* NvCloth = static_cast<nv::cloth::Cloth*>(Cloth);
	const uint32_t OldSpheres = NvCloth->getNumSpheres();
	if (Spheres && SphereCount > 0)
	{
		std::vector<physx::PxVec4> PxSpheres(SphereCount);
		for (uint32_t i = 0; i < SphereCount; ++i)
		{
			PxSpheres[i] = physx::PxVec4(Spheres[i].X, Spheres[i].Y, Spheres[i].Z, Spheres[i].W);
		}
		NvCloth->setSpheres(nv::cloth::Range<const physx::PxVec4>(PxSpheres.data(), PxSpheres.data() + PxSpheres.size()), 0, OldSpheres);
	}
	else if (OldSpheres > 0)
	{
		NvCloth->setSpheres(nv::cloth::Range<const physx::PxVec4>(), 0, OldSpheres);
	}

	const uint32_t OldCapsules = NvCloth->getNumCapsules();
	if (CapsuleIndices && CapsuleIndexCount >= 2)
	{
		NvCloth->setCapsules(nv::cloth::Range<const uint32_t>(CapsuleIndices, CapsuleIndices + CapsuleIndexCount), 0, OldCapsules);
	}
	else if (OldCapsules > 0)
	{
		NvCloth->setCapsules(nv::cloth::Range<const uint32_t>(), 0, OldCapsules);
	}
}

void ClothBridge_SetVirtualParticles(FClothInstanceHandle Cloth, const uint32_t* TriangleIndices, uint32_t TriangleCount)
{
	if (!Cloth)
	{
		return;
	}
	nv::cloth::Cloth* NvCloth = static_cast<nv::cloth::Cloth*>(Cloth);
	if (!TriangleIndices || TriangleCount == 0)
	{
		NvCloth->setVirtualParticles(nv::cloth::Range<const uint32_t[4]>(), nv::cloth::Range<const physx::PxVec3>());
		return;
	}

	const uint32_t ParticleCount = NvCloth->getNumParticles();
	std::vector<uint32_t> PackedIndices;
	PackedIndices.reserve(static_cast<size_t>(TriangleCount) * 4u);
	for (uint32_t i = 0; i < TriangleCount; ++i)
	{
		const uint32_t I0 = TriangleIndices[i * 3u];
		const uint32_t I1 = TriangleIndices[i * 3u + 1u];
		const uint32_t I2 = TriangleIndices[i * 3u + 2u];
		if (I0 < ParticleCount && I1 < ParticleCount && I2 < ParticleCount)
		{
			PackedIndices.push_back(I0);
			PackedIndices.push_back(I1);
			PackedIndices.push_back(I2);
			PackedIndices.push_back(0u);
		}
	}

	if (PackedIndices.empty())
	{
		NvCloth->setVirtualParticles(nv::cloth::Range<const uint32_t[4]>(), nv::cloth::Range<const physx::PxVec3>());
		return;
	}

	const physx::PxVec3 CentroidWeight(1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f);
	const uint32_t ValidTris = static_cast<uint32_t>(PackedIndices.size() / 4u);
	const uint32_t (*TriPtr)[4] = reinterpret_cast<const uint32_t (*)[4]>(PackedIndices.data());
	NvCloth->setVirtualParticles(
		nv::cloth::Range<const uint32_t[4]>(TriPtr, TriPtr + ValidTris),
		nv::cloth::Range<const physx::PxVec3>(&CentroidWeight, &CentroidWeight + 1));
}

void ClothBridge_SetMotionConstraints(FClothInstanceHandle Cloth, const FClothBridgeVec4* Constraints, uint32_t Count)
{
	if (!Cloth)
	{
		return;
	}
	nv::cloth::Cloth* NvCloth = static_cast<nv::cloth::Cloth*>(Cloth);
	if (!Constraints || Count == 0)
	{
		NvCloth->clearMotionConstraints();
		return;
	}
	nv::cloth::Range<physx::PxVec4> Dst = NvCloth->getMotionConstraints();
	const uint32_t DstCount = static_cast<uint32_t>(Dst.size());
	const uint32_t CopyCount = (Count < DstCount) ? Count : DstCount;
	for (uint32_t i = 0; i < CopyCount; ++i)
	{
		Dst[i] = physx::PxVec4(Constraints[i].X, Constraints[i].Y, Constraints[i].Z, Constraints[i].W);
	}
}

void ClothBridge_SetSeparationConstraints(FClothInstanceHandle Cloth, const FClothBridgeVec4* Constraints, uint32_t Count)
{
	if (!Cloth)
	{
		return;
	}
	nv::cloth::Cloth* NvCloth = static_cast<nv::cloth::Cloth*>(Cloth);
	if (!Constraints || Count == 0)
	{
		NvCloth->clearSeparationConstraints();
		return;
	}
	nv::cloth::Range<physx::PxVec4> Dst = NvCloth->getSeparationConstraints();
	const uint32_t DstCount = static_cast<uint32_t>(Dst.size());
	const uint32_t CopyCount = (Count < DstCount) ? Count : DstCount;
	for (uint32_t i = 0; i < CopyCount; ++i)
	{
		Dst[i] = physx::PxVec4(Constraints[i].X, Constraints[i].Y, Constraints[i].Z, Constraints[i].W);
	}
}

void ClothBridge_ClearInterpolation(FClothInstanceHandle Cloth)
{
	if (Cloth)
	{
		static_cast<nv::cloth::Cloth*>(Cloth)->clearInterpolation();
	}
}

void ClothBridge_WriteParticles(FClothInstanceHandle Cloth, const FClothBridgeVec4* Particles, uint32_t Count)
{
	if (!Cloth || !Particles || Count == 0)
	{
		return;
	}
	nv::cloth::Cloth* NvCloth = static_cast<nv::cloth::Cloth*>(Cloth);
	nv::cloth::MappedRange<physx::PxVec4> Range = NvCloth->getCurrentParticles();
	nv::cloth::MappedRange<physx::PxVec4> PrevRange = NvCloth->getPreviousParticles();
	const uint32_t RangeCount = static_cast<uint32_t>(Range.size());
	const uint32_t CopyCount = (Count < RangeCount) ? Count : RangeCount;
	for (uint32_t i = 0; i < CopyCount; ++i)
	{
		const physx::PxVec4 Value(Particles[i].X, Particles[i].Y, Particles[i].Z, Particles[i].W);
		Range[i] = Value;
		if (i < PrevRange.size())
		{
			PrevRange[i] = Value;
		}
	}
}

void ClothBridge_WriteParticlesWithPrevious(
	FClothInstanceHandle Cloth,
	const FClothBridgeVec4* CurrentParticles,
	const FClothBridgeVec4* PreviousParticles,
	uint32_t Count)
{
	if (!Cloth || !CurrentParticles || Count == 0)
	{
		return;
	}
	nv::cloth::Cloth* NvCloth = static_cast<nv::cloth::Cloth*>(Cloth);
	nv::cloth::MappedRange<physx::PxVec4> CurRange = NvCloth->getCurrentParticles();
	nv::cloth::MappedRange<physx::PxVec4> PrevRange = NvCloth->getPreviousParticles();
	const uint32_t RangeCount = static_cast<uint32_t>(CurRange.size());
	const uint32_t CopyCount = (Count < RangeCount) ? Count : RangeCount;
	for (uint32_t i = 0; i < CopyCount; ++i)
	{
		CurRange[i] = physx::PxVec4(CurrentParticles[i].X, CurrentParticles[i].Y, CurrentParticles[i].Z, CurrentParticles[i].W);
		if (i < PrevRange.size())
		{
			const FClothBridgeVec4& PrevVal = PreviousParticles ? PreviousParticles[i] : CurrentParticles[i];
			PrevRange[i] = physx::PxVec4(PrevVal.X, PrevVal.Y, PrevVal.Z, PrevVal.W);
		}
	}
}

void ClothBridge_UpdatePinnedParticles(FClothInstanceHandle Cloth, const uint32_t* Indices, const FClothBridgeVec3* LocalPositions, uint32_t Count)
{
	if (!Cloth || !Indices || !LocalPositions || Count == 0)
	{
		return;
	}
	nv::cloth::Cloth* NvCloth = static_cast<nv::cloth::Cloth*>(Cloth);
	nv::cloth::MappedRange<physx::PxVec4> Range = NvCloth->getCurrentParticles();
	const uint32_t RangeCount = static_cast<uint32_t>(Range.size());
	for (uint32_t i = 0; i < Count; ++i)
	{
		const uint32_t Idx = Indices[i];
		if (Idx < RangeCount)
		{
			Range[Idx] = physx::PxVec4(LocalPositions[i].X, LocalPositions[i].Y, LocalPositions[i].Z, 0.0f);
		}
	}
}

void ClothBridge_ReadParticles(FClothInstanceHandle Cloth, FClothBridgeVec4* OutParticles, uint32_t Count)
{
	if (!Cloth || !OutParticles || Count == 0)
	{
		return;
	}
	const nv::cloth::Cloth* NvCloth = static_cast<const nv::cloth::Cloth*>(Cloth);
	nv::cloth::MappedRange<const physx::PxVec4> Range = nv::cloth::readCurrentParticles(*NvCloth);
	const uint32_t RangeCount = static_cast<uint32_t>(Range.size());
	const uint32_t CopyCount = (Count < RangeCount) ? Count : RangeCount;
	for (uint32_t i = 0; i < CopyCount; ++i)
	{
		OutParticles[i].X = Range[i].x;
		OutParticles[i].Y = Range[i].y;
		OutParticles[i].Z = Range[i].z;
		OutParticles[i].W = Range[i].w;
	}
}

void ClothBridge_ReadPreviousParticles(FClothInstanceHandle Cloth, FClothBridgeVec4* OutParticles, uint32_t Count)
{
	if (!Cloth || !OutParticles || Count == 0)
	{
		return;
	}
	const nv::cloth::Cloth* NvCloth = static_cast<const nv::cloth::Cloth*>(Cloth);
	nv::cloth::MappedRange<const physx::PxVec4> Range = nv::cloth::readPreviousParticles(*NvCloth);
	const uint32_t RangeCount = static_cast<uint32_t>(Range.size());
	const uint32_t CopyCount = (Count < RangeCount) ? Count : RangeCount;
	for (uint32_t i = 0; i < CopyCount; ++i)
	{
		OutParticles[i].X = Range[i].x;
		OutParticles[i].Y = Range[i].y;
		OutParticles[i].Z = Range[i].z;
		OutParticles[i].W = Range[i].w;
	}
}

#include "EnginePCH.h"
#include "BodyInstance.h"

#include "BodySetup.h"
#include "Render/StaticMeshData.h"
#include "PhysicsCore.h"
#include "PhysXConversion.h"
#include "PhysScene.h"
#include <PxPhysics.h>
#include <PxRigidDynamic.h>
#include <PxRigidStatic.h>
#include <PxScene.h>
#include <cooking/PxCooking.h>
#include <cooking/PxTriangleMeshDesc.h>
#include <geometry/PxTriangleMesh.h>
#include <geometry/PxTriangleMeshGeometry.h>
#include <extensions/PxRigidActorExt.h>
#include <extensions/PxRigidBodyExt.h>

using namespace physx;

namespace
{
	constexpr float DefaultDensity = 1000.0f;

	constexpr float RaiseMassToPower = 0.75f;

	const PxQuat SphylBasis(PxHalfPi, PxVec3(0.0f, 1.0f, 0.0f));

	FVector Abs(const FVector& V) { return { std::abs(V.X), std::abs(V.Y), std::abs(V.Z) }; }
	FVector Mul(const FVector& A, const FVector& B) { return { A.X * B.X, A.Y * B.Y, A.Z * B.Z }; }

	PxTransform MakeLocalPose(const FShapeElem& Elem, const FVector& Scale)
	{
		return { K2PVector(Mul(Elem.Center, Scale)), K2PQuat(Elem.Rotation.Quaternion().Normalized()) };
	}

	void ApplyCollisionFlags(PxShape* Shape, ECollisionEnabled Enabled)
	{
		const bool bSim = Enabled == ECollisionEnabled::PhysicsOnly || Enabled == ECollisionEnabled::QueryAndPhysics;
		const bool bQuery = Enabled == ECollisionEnabled::QueryOnly || Enabled == ECollisionEnabled::QueryAndPhysics;
		const bool bTrigger = Enabled == ECollisionEnabled::QueryOnly;

		Shape->setFlag(PxShapeFlag::eSIMULATION_SHAPE, bSim);
		Shape->setFlag(PxShapeFlag::eSCENE_QUERY_SHAPE, bQuery);
		Shape->setFlag(PxShapeFlag::eTRIGGER_SHAPE, bTrigger);
	}

	int32 CreateShapes(PxRigidActor& Actor, const FAggregateGeom& Geom, const FVector& InScale, ECollisionEnabled Enabled)
	{
		PxMaterial& Material = *FPhysicsCore::GetDefaultMaterial();
		const FVector S = Abs(InScale);
		int32 Count = 0;

		auto Attach = [&](const PxGeometry& Geometry, const PxTransform& LocalPose)
		{
			if (PxShape* Shape = PxRigidActorExt::createExclusiveShape(Actor, Geometry, Material))
			{
				Shape->setLocalPose(LocalPose);
				ApplyCollisionFlags(Shape, Enabled);
				++Count;
			}
		};

		for (const FSphereElem& Element : Geom.SphereElems)
		{
			const float Radius = Element.Radius * std::max({ S.X, S.Y, S.Z });
			Attach(PxSphereGeometry(Radius), MakeLocalPose(Element, InScale));
		}

		for (const FBoxElem& Element : Geom.BoxElems)
		{
			const PxVec3 HalfExtents(0.5f * Element.X * S.X, 0.5f * Element.Y * S.Y, 0.5f * Element.Z * S.Z);
			Attach(PxBoxGeometry(HalfExtents), MakeLocalPose(Element, InScale));
		}

		for (const FCapsuleElem& Element : Geom.CapsuleElems)
		{
			const float Radius = Element.Radius * FMath::Max(S.X, S.Y);
			const float HalfHeight = FMath::Max(0.0f, (0.5f * Element.Length + Element.Radius) * S.Z - Radius);
			PxTransform Pose = MakeLocalPose(Element, InScale);
			Pose.q = Pose.q * SphylBasis;
			Attach(PxCapsuleGeometry(Radius, HalfHeight), Pose);
		}

		return Count;
	}
}

void FBodyInstance::InitBody(UBodySetup* InBodySetup, const FTransform& WorldTransform, UPrimitiveComponent* Owner, FPhysScene* InScene, PxAggregate* Aggregate)
{
	if (RigidActor)
	{
		return;
	}

	PxPhysics* Physics = FPhysicsCore::GetPhysics();
	PxScene* Scene = InScene ? InScene->GetPxScene() : nullptr;
	if (!Physics || !Scene || !InBodySetup || !Owner)
	{
		return;
	}

	const PxTransform Pose{
		K2PVector(WorldTransform.Location),
		K2PQuat(WorldTransform.Rotation.Normalized()),
	};
	if (bUseTriangleMesh && bSimulatePhysics)
	{
		KRS_LOG(Warning, "[PhysX] bUseTriangleMesh is incompatible with bSimulatePhysics; falling back to simple bounds");
	}

	PxRigidActor* CreatedActor = nullptr;
	if (bUseTriangleMesh && !bSimulatePhysics && InBodySetup->TriangleMesh)
	{
		PxRigidStatic* Static = Physics->createRigidStatic(Pose);
		if (!Static)
		{
			KRS_LOG(Error, "[PhysX] createRigidStatic failed");
			return;
		}

		const FVector S = Abs(WorldTransform.Scale);
		const PxMeshScale MeshScale(PxVec3(
			std::max(S.X, 1e-4f),
			std::max(S.Y, 1e-4f),
			std::max(S.Z, 1e-4f)));
		const PxTriangleMeshGeometry TriGeom(
			InBodySetup->TriangleMesh,
			MeshScale,
			PxMeshGeometryFlag::eDOUBLE_SIDED);

		PxShape* Shape = PxRigidActorExt::createExclusiveShape(*Static, TriGeom, *FPhysicsCore::GetDefaultMaterial());
		if (!Shape)
		{
			Static->release();
			KRS_LOG(Error, "[PhysX] createExclusiveShape failed for TriangleMesh");
			return;
		}
		ApplyCollisionFlags(Shape, CollisionEnabled);
		CreatedActor = Static;
	}
	else
	{
		PxRigidDynamic* Dynamic = Physics->createRigidDynamic(Pose);
		if (!Dynamic)
		{
			KRS_LOG(Error, "[PhysX] createRigidDynamic failed");
			return;
		}
		Dynamic->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC, !bSimulatePhysics); // 두번째 인자가 true면 물리에 의해 움직임, false면 게임 로직에 의해 움직임
		if (BoneIndex != INDEX_NONE)
		{
			Dynamic->setSolverIterationCounts(12, 4);
			Dynamic->setMaxDepenetrationVelocity(2.0f);
			Dynamic->setMaxAngularVelocity(12.0f);
		}

		if (CreateShapes(*Dynamic, InBodySetup->AggGeom, WorldTransform.Scale, CollisionEnabled) == 0)
		{
			Dynamic->release();
			return;
		}

		if (MassInKgOverride > 0.0f)
		{
			PxRigidBodyExt::setMassAndUpdateInertia(*Dynamic, MassInKgOverride);
		}
		else
		{
			PxRigidBodyExt::updateMassAndInertia(*Dynamic, DefaultDensity);
			const float DensityMass = Dynamic->getMass();
			if (DensityMass > 0.0f)
			{
				PxRigidBodyExt::setMassAndUpdateInertia(*Dynamic, std::pow(DensityMass, RaiseMassToPower));
			}
		}

		Dynamic->setLinearDamping(LinearDamping); // 선속도 감쇠 계수
		Dynamic->setAngularDamping(AngularDamping); // 각속도 감쇠 계수
		Dynamic->setActorFlag(PxActorFlag::eDISABLE_GRAVITY, !bEnableGravity);
		CreatedActor = Dynamic;
	}

	CreatedActor->userData = this;

	if (Aggregate)
	{
		Aggregate->addActor(*CreatedActor);
	}
	else
	{
		Scene->addActor(*CreatedActor);
	}

	RigidActor = CreatedActor;
	BodySetup = InBodySetup;
	OwnerComponent = Owner;
	PhysScene = InScene;
	Scale3D = WorldTransform.Scale;
}

void FBodyInstance::InitStaticTriangleMeshBody(const FStaticMeshData& Mesh, const FTransform& WorldTransform, UPrimitiveComponent* Owner, FPhysScene* InScene)
{
	if (RigidActor)
	{
		return;
	}

	PxPhysics* Physics = FPhysicsCore::GetPhysics();
	PxScene* Scene = InScene ? InScene->GetPxScene() : nullptr;
	if (!Physics || !Scene || !Owner || Mesh.Vertices.IsEmpty() || Mesh.Indices.Num() < 3)
	{
		return;
	}

	TArray<PxVec3> Points;
	Points.Reserve(Mesh.Vertices.Num());
	for (const FVertex& Vertex : Mesh.Vertices)
	{
		Points.Add(K2PVector(Vertex.Position));
	}

	PxTriangleMeshDesc Desc;
	Desc.points.count = static_cast<PxU32>(Points.Num());
	Desc.points.stride = sizeof(PxVec3);
	Desc.points.data = Points.GetData();
	Desc.triangles.count = static_cast<PxU32>(Mesh.Indices.Num() / 3);
	Desc.triangles.stride = 3 * sizeof(uint32);
	Desc.triangles.data = Mesh.Indices.GetData();

	const PxCookingParams Params(Physics->getTolerancesScale());
	PxTriangleMeshCookingResult::Enum CookResult = PxTriangleMeshCookingResult::eSUCCESS;
	PxTriangleMesh* TriMesh = PxCreateTriangleMesh(Params, Desc, Physics->getPhysicsInsertionCallback(), &CookResult);
	if (!TriMesh)
	{
		KRS_LOG(Error, "[PhysX] triangle mesh cooking failed ({})", static_cast<int32>(CookResult));
		return;
	}

	PxRigidStatic* Static = Physics->createRigidStatic(PxTransform(
		K2PVector(WorldTransform.Location), K2PQuat(WorldTransform.Rotation.Normalized())));
	if (!Static)
	{
		TriMesh->release();
		return;
	}

	const PxMeshScale MeshScale(K2PVector(WorldTransform.Scale));
	PxShape* Shape = PxRigidActorExt::createExclusiveShape(*Static, PxTriangleMeshGeometry(TriMesh, MeshScale), *FPhysicsCore::GetDefaultMaterial());
	TriMesh->release();
	if (!Shape)
	{
		Static->release();
		return;
	}
	ApplyCollisionFlags(Shape, CollisionEnabled);

	Static->userData = this;
	Scene->addActor(*Static);

	RigidActor = Static;
	BodySetup = nullptr;
	OwnerComponent = Owner;
	PhysScene = InScene;
	Scale3D = WorldTransform.Scale;
	bSimulatePhysics = false;
}

void FBodyInstance::TermBody()
{
	if (RigidActor)
	{
		RigidActor->userData = nullptr;
		RigidActor->release();
		RigidActor = nullptr;
	}
	BodySetup = nullptr;
	OwnerComponent = nullptr;
	PhysScene = nullptr;
}

physx::PxRigidDynamic* FBodyInstance::GetPxRigidDynamic() const
{
	return RigidActor ? RigidActor->is<PxRigidDynamic>() : nullptr;
}

bool FBodyInstance::IsDynamic() const
{
	const PxRigidDynamic* Dynamic = GetPxRigidDynamic();
	return Dynamic && !(Dynamic->getRigidBodyFlags() & PxRigidBodyFlag::eKINEMATIC);
}

void FBodyInstance::SetBodyTransform(const FTransform& WorldTransform, ETeleportType TeleportType)
{
	if (!RigidActor)
	{
		return;
	}

	const PxTransform Pose{
		K2PVector(WorldTransform.Location),
		K2PQuat(WorldTransform.Rotation.Normalized()),
	};

	PxRigidDynamic* Dynamic = GetPxRigidDynamic();
	if (!Dynamic)
	{
		RigidActor->setGlobalPose(Pose);
		return;
	}

	const bool bKinematic = Dynamic->getRigidBodyFlags() & PxRigidBodyFlag::eKINEMATIC;

	if (bKinematic && TeleportType == ETeleportType::None)
	{
		Dynamic->setKinematicTarget(Pose);
	}
	else
	{
		Dynamic->setGlobalPose(Pose);
	}
}

FTransform FBodyInstance::GetUnrealWorldTransform() const
{
	if (!RigidActor)
	{
		return FTransform::Identity;
	}
	
	const PxTransform Pose = RigidActor->getGlobalPose();
	return FTransform{
		P2KQuat(Pose.q),
		P2KVector(Pose.p), 
		Scale3D
	};
}

void FBodyInstance::SetInstanceSimulatePhysics(bool bSimulate)
{
	bSimulatePhysics = bSimulate;

	PxRigidDynamic* Dynamic = GetPxRigidDynamic();
	if (!Dynamic)
	{
		return;
	}

	Dynamic->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC, !bSimulate);
	if (bSimulate && Dynamic->getScene())
	{
		Dynamic->wakeUp();
	}
}

void FBodyInstance::SetCollisionEnabled(ECollisionEnabled InCollisionEnabled)
{
	CollisionEnabled = InCollisionEnabled;
	if (!RigidActor)
		return;

	const PxU32 ShapeCount = RigidActor->getNbShapes();
	TArray<PxShape*> Shapes;
	Shapes.SetNum(static_cast<int32>(ShapeCount));
	RigidActor->getShapes(Shapes.GetData(), ShapeCount);
	for (PxShape* Shape : Shapes)
		ApplyCollisionFlags(Shape, CollisionEnabled);
}

void FBodyInstance::AddImpulse(const FVector& Impulse, bool bVelocityChange)
{
	if (!IsDynamic())
	{
		return;
	}

	GetPxRigidDynamic()->addForce(K2PVector(Impulse), bVelocityChange ? PxForceMode::eVELOCITY_CHANGE : PxForceMode::eIMPULSE);
}

void FBodyInstance::AddForce(const FVector& Force)
{
	if (!IsDynamic())
	{
		return;
	}

	GetPxRigidDynamic()->addForce(K2PVector(Force), PxForceMode::eFORCE);
}

void FBodyInstance::SetLinearVelocity(const FVector& Velocity)
{
	if (IsDynamic())
	{
		GetPxRigidDynamic()->setLinearVelocity(K2PVector(Velocity));
	}
}

FVector FBodyInstance::GetLinearVelocity() const
{
	const PxRigidDynamic* Dynamic = GetPxRigidDynamic();
	return Dynamic ? P2KVector(Dynamic->getLinearVelocity()) : FVector::ZeroVector;
}

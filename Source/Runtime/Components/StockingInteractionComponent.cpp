#include "EnginePCH.h"
#include "Components/StockingInteractionComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Input/InputSystem.h"
#include "Render/SkeletalMeshObject.h"
#include "UObject/UObjectIterator.h"

#include <cfloat>
#include <cmath>

namespace
{
	// 광선과 삼각형 교차 거리 및 무게중심 좌표 계산
	bool IntersectRayTriangleBarycentric(
		const FRay& Ray,
		const FVector& V0,
		const FVector& V1,
		const FVector& V2,
		float& OutT,
		FVector& OutBarycentric)
	{
		const float Epsilon = 1e-7f;
		const FVector Edge1 = V1 - V0;
		const FVector Edge2 = V2 - V0;
		const FVector PVec = FVector::Cross(Ray.Direction, Edge2);
		const float Det = Edge1.Dot(PVec);

		if (std::fabs(Det) < Epsilon)
		{
			return false;
		}

		const float InvDet = 1.0f / Det;
		const FVector TVec = Ray.Origin - V0;
		const float U = TVec.Dot(PVec) * InvDet;
		if (U < 0.0f || U > 1.0f)
		{
			return false;
		}

		const FVector QVec = FVector::Cross(TVec, Edge1);
		const float V = Ray.Direction.Dot(QVec) * InvDet;
		if (V < 0.0f || (U + V) > 1.0f)
		{
			return false;
		}

		const float T = Edge2.Dot(QVec) * InvDet;
		if (T <= Epsilon)
		{
			return false;
		}

		OutT = T;
		OutBarycentric = FVector(1.0f - U - V, U, V);
		return true;
	}
}

UStockingInteractionComponent::UStockingInteractionComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = ETickingGroup::PrePhysics;
}

void UStockingInteractionComponent::TickComponent(float DeltaTime)
{
	Super::TickComponent(DeltaTime);

	const UWorld* World = GetWorld();
	if (!World || !World->IsGameWorld())
	{
		return;
	}

	// 인게임 뷰포트 마우스 당기기 입력 비활성화
	if (bIsGrabbing)
	{
		EndGrab();
	}
}

bool UStockingInteractionComponent::RaycastSingleStocking(
	UStockingComponent* Stocking,
	const FRay& WorldRay,
	float MaxDistance,
	FStockingHitInfo& OutHit)
{
	if (!Stocking || !Stocking->IsVisible())
	{
		return false;
	}

	USkeletalMesh* Mesh = Stocking->GetSkeletalMesh();
	FSkeletalMeshObject* MeshObject = Stocking->GetMeshObject();
	if (!Mesh || !MeshObject || Mesh->GetRenderData().LODRenderData.IsEmpty())
	{
		return false;
	}

	const TArray<FVertex>* Vertices = MeshObject->GetCPUSkinnedVertices();
	if (!Vertices || Vertices->IsEmpty())
	{
		return false;
	}

	const FMatrix WorldMatrix = Stocking->GetWorldMatrix();
	const FRay LocalRay = ToLocalRay(WorldRay, WorldMatrix);
	const TArray<uint32>& Indices = Mesh->GetRenderData().LODRenderData[0].Indices;

	float BestT = MaxDistance;
	bool bFound = false;
	uint32 BestTri = 0;
	FVector BestBary(1.0f, 0.0f, 0.0f);
	FVector BestLocalNormal(0.0f, 0.0f, 1.0f);

	const int32 TriCount = Indices.Num() / 3;
	for (int32 t = 0; t < TriCount; ++t)
	{
		const uint32 I0 = Indices[t * 3];
		const uint32 I1 = Indices[t * 3 + 1];
		const uint32 I2 = Indices[t * 3 + 2];

		const FVector& P0 = (*Vertices)[I0].Position;
		const FVector& P1 = (*Vertices)[I1].Position;
		const FVector& P2 = (*Vertices)[I2].Position;

		float HitT = FLT_MAX;
		FVector HitBary;
		if (IntersectRayTriangleBarycentric(LocalRay, P0, P1, P2, HitT, HitBary) && HitT < BestT)
		{
			BestT = HitT;
			BestTri = static_cast<uint32>(t);
			BestBary = HitBary;
			const FVector SmoothNormal =
				(*Vertices)[I0].Normal * HitBary.X +
				(*Vertices)[I1].Normal * HitBary.Y +
				(*Vertices)[I2].Normal * HitBary.Z;
			BestLocalNormal = SmoothNormal.SizeSquared() > 1e-10f ? SmoothNormal.Normalized() : FVector(0.0f, 0.0f, 1.0f);
			bFound = true;
		}
	}

	if (!bFound)
	{
		return false;
	}

	OutHit.HitComponent = Stocking;
	OutHit.TriangleIndex = BestTri;
	OutHit.Barycentric = BestBary;
	OutHit.LocalLocation = LocalRay.PointAt(BestT);
	OutHit.WorldLocation = WorldMatrix.TransformPosition(OutHit.LocalLocation);
	OutHit.WorldNormal = WorldMatrix.TransformVector(BestLocalNormal).Normalized();
	OutHit.Distance = BestT;
	return true;
}

bool UStockingInteractionComponent::RaycastStocking(const FRay& WorldRay, FStockingHitInfo& OutHit) const
{
	if (TargetStockingComponent && IsValid(TargetStockingComponent))
	{
		return RaycastSingleStocking(TargetStockingComponent, WorldRay, MaxTraceDistance, OutHit);
	}

	if (UStockingComponent* OwnerStocking = ResolveOwnerStocking())
	{
		if (RaycastSingleStocking(OwnerStocking, WorldRay, MaxTraceDistance, OutHit))
		{
			return true;
		}
	}

	if (!bSearchAllWorldStockings)
	{
		return false;
	}

	UWorld* World = GetWorld();
	float BestDist = MaxTraceDistance;
	bool bHitAny = false;
	for (TObjectIterator<UStockingComponent> It; It; ++It)
	{
		UStockingComponent* Candidate = *It;
		if (!Candidate || !Candidate->IsRegistered() || (World && Candidate->GetWorld() != World))
		{
			continue;
		}
		FStockingHitInfo CandidateHit;
		if (RaycastSingleStocking(Candidate, WorldRay, BestDist, CandidateHit))
		{
			BestDist = CandidateHit.Distance;
			OutHit = CandidateHit;
			bHitAny = true;
		}
	}
	return bHitAny;
}

bool UStockingInteractionComponent::BeginGrabFromRay(const FRay& WorldRay)
{
	EndGrab();

	FStockingHitInfo Hit;
	if (!RaycastStocking(WorldRay, Hit) || !Hit.HitComponent)
	{
		return false;
	}

	ActiveStocking = Hit.HitComponent;
	bIsGrabbing = true;
	DragPlanePoint = Hit.WorldLocation;
	const FVector RayDir = WorldRay.Direction.Normalized();
	DragPlaneNormal = RayDir.SizeSquared() > 1e-8f ? (RayDir * -1.0f) : FVector(1.0f, 0.0f, 0.0f);
	CurrentDragWorldPosition = Hit.WorldLocation;

	ActiveStocking->BeginPull(Hit.TriangleIndex, Hit.Barycentric, CurrentDragWorldPosition);
	return true;
}

void UStockingInteractionComponent::UpdateGrabFromRay(const FRay& WorldRay)
{
	if (!bIsGrabbing || !ActiveStocking || !IsValid(ActiveStocking))
	{
		EndGrab();
		return;
	}

	float PlaneT = 0.0f;
	if (RayIntersectsPlane(WorldRay, DragPlanePoint, DragPlaneNormal, PlaneT) && PlaneT > 0.0f)
	{
		CurrentDragWorldPosition = WorldRay.PointAt(PlaneT);
		ActiveStocking->UpdatePull(CurrentDragWorldPosition);
	}
}

void UStockingInteractionComponent::EndGrab()
{
	if (ActiveStocking && IsValid(ActiveStocking))
	{
		ActiveStocking->EndPull();
	}
	bIsGrabbing = false;
	ActiveStocking = nullptr;
}

bool UStockingInteractionComponent::BuildGameViewportRay(FRay& OutWorldRay) const
{
	UWorld* World = GetWorld();
	if (!GEngine || !World)
	{
		return false;
	}

	FWorldContext* Context = GEngine->GetWorldContextFromWorld(World);
	if (!Context || !Context->GameViewport)
	{
		return false;
	}

	const float MouseX = static_cast<float>(FInputSystem::GetMouseX());
	const float MouseY = static_cast<float>(FInputSystem::GetMouseY());
	return Context->GameViewport->ScreenToWorldRay(MouseX, MouseY, &OutWorldRay);
}

UStockingComponent* UStockingInteractionComponent::ResolveOwnerStocking() const
{
	AActor* OwnerActor = GetOwner();
	if (!OwnerActor)
	{
		return nullptr;
	}

	for (UActorComponent* Comp : OwnerActor->GetComponents())
	{
		if (UStockingComponent* Stocking = Cast<UStockingComponent>(Comp))
		{
			return Stocking;
		}
	}
	return nullptr;
}

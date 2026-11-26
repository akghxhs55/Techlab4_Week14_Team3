#include "EnginePCH.h"
#include "StaticMeshComponent.h"
#include "Asset/AssetManager.h"
#include "Render/RenderCommand.h"

#include "Engine/PrimitiveSceneProxy.h"
#include "Engine/World.h"


// StaticMesh 컴포넌트를 초기화한다.
UStaticMeshComponent::UStaticMeshComponent()
{
	StaticMesh = UAssetManager::GetAssetByPath<UStaticMesh>("Cube");
}

// StaticMesh 컴포넌트의 소멸을 처리한다.
UStaticMeshComponent::~UStaticMeshComponent()
{
}

// 부모 컴포넌트의 시작 처리를 호출한다.
void UStaticMeshComponent::BeginPlay()
{
	Super::BeginPlay();

	if (IsPhysicsStateCreated() && PhysicsBodyMesh != StaticMesh)
	{
		OnDestroyPhysicsState();
		OnCreatePhysicsState();
	}
}

// 부모 컴포넌트의 프레임 갱신을 호출한다.
void UStaticMeshComponent::TickComponent(float DeltaTime)
{
	Super::TickComponent(DeltaTime);
}

void UStaticMeshComponent::OnCreatePhysicsState()
{
	PhysicsBodyMesh = StaticMesh;
	if (bUseComplexAsSimpleCollision && StaticMesh && !bEditorIcon && !BodyInstance.bSimulatePhysics)
	{
		BodyInstance.InitStaticTriangleMeshBody(StaticMesh->GetMeshData(), { GetWorldRotation(), GetWorldLocation(), GetWorldScale3D() }, this, GetWorld()->GetPhysicsScene());
		return;
	}
	Super::OnCreatePhysicsState();
}

void UStaticMeshComponent::SetStaticMesh(UStaticMesh* InStaticMesh)
{
	if (StaticMesh == InStaticMesh)
		return;

	const bool bRecreatePhysics = IsPhysicsStateCreated();
	if (bRecreatePhysics)
		OnDestroyPhysicsState();

	StaticMesh = InStaticMesh;
	ClearOverrideMaterials();
	MarkRenderStateDirty();
	OnTransformDirty(ETeleportType::None);

	if (bRecreatePhysics && ShouldCreatePhysicsState())
		OnCreatePhysicsState();
}

UBodySetup* UStaticMeshComponent::GetBodySetup()
{
	if (!StaticMesh || bEditorIcon)
		return nullptr;

	UBodySetup* Setup = StaticMesh->GetBodySetup();
	if (Setup && BodyInstance.bUseTriangleMesh && !BodyInstance.bSimulatePhysics)
	{
		StaticMesh->EnsureTriangleMesh();
	}
	return Setup;
}

int32 UStaticMeshComponent::GetNumMaterials() const
{
	return StaticMesh ? static_cast<int32>(StaticMesh->GetMeshData().MaterialSlots.Num()) : 0;
}

FString UStaticMeshComponent::GetMaterialSlotName(int32 SlotIndex) const
{
	if (!StaticMesh || SlotIndex < 0 || SlotIndex >= GetNumMaterials())
		return FString();
	return StaticMesh->GetMeshData().MaterialSlots[SlotIndex].Name;
}

UMaterialInterface* UStaticMeshComponent::GetDefaultMaterial(int32 SlotIndex) const
{
	return StaticMesh ? StaticMesh->GetMaterial(static_cast<uint32>(SlotIndex)) : nullptr;
}

FPrimitiveSceneProxy* UStaticMeshComponent::CreateSceneProxy()
{
	return new FStaticMeshSceneProxy(this);
}

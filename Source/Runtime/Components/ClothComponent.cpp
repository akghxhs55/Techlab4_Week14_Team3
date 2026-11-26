#include "EnginePCH.h"
#include "Components/ClothComponent.h"

#include "Asset/AssetManager.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/ClothSceneProxy.h"
#include "Engine/World.h"
#include "Physics/ClothCore.h"
#include "Physics/PhysScene.h"
#include "Render/Mesh.h"
#include "Render/RenderCommand.h"
#include "Render/SkeletalMesh.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>

UClothComponent::UClothComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
}

UClothComponent::~UClothComponent()
{
	DestroyNvClothInstance();
}

void UClothComponent::BeginPlay()
{
	Super::BeginPlay();
}

void UClothComponent::TickComponent(float DeltaTime)
{
	Super::TickComponent(DeltaTime);
}

FString UClothComponent::GetMaterialSlotName(int32 SlotIndex) const
{
	if (SlotIndex == 0 && SourceStaticMesh && !SourceStaticMesh->GetMeshData().MaterialSlots.IsEmpty())
	{
		return SourceStaticMesh->GetMeshData().MaterialSlots[0].Name;
	}
	return SlotIndex == 0 ? FString("ClothMaterial") : FString();
}

UMaterialInterface* UClothComponent::GetDefaultMaterial(int32 SlotIndex) const
{
	if (SlotIndex != 0)
	{
		return nullptr;
	}
	if (SourceStaticMesh)
	{
		return SourceStaticMesh->GetMaterial(0);
	}
	return UAssetManager::GetAssetByPath<UMaterial>("DefaultMaterial");
}

FBox UClothComponent::CalcLocalBounds() const
{
	return LocalBounds;
}

FPrimitiveSceneProxy* UClothComponent::CreateSceneProxy()
{
	return new FClothSceneProxy(this);
}

void UClothComponent::PostEditChangeProperty(const FProperty& Property)
{
	Super::PostEditChangeProperty(Property);

	const FString& PropName = Property.Name;
	const bool bGeometryChanged =
		PropName == "Preset" ||
		PropName == "SourceStaticMesh" ||
		PropName == "ClothWidth" ||
		PropName == "BottomWidth" ||
		PropName == "ClothHeight" ||
		PropName == "TopRadius" ||
		PropName == "BottomRadius" ||
		PropName == "SegmentsX" ||
		PropName == "SegmentsY" ||
		PropName == "bDoubleSidedMesh" ||
		PropName == "bPinTopRow" ||
		PropName == "PinHeightRatio";

	if (bGeometryChanged)
	{
		const bool bHadPhysics = IsPhysicsStateCreated();
		if (bHadPhysics)
		{
			OnDestroyPhysicsState();
		}
		RebuildClothMesh();
		if (bHadPhysics && ShouldCreatePhysicsState())
		{
			OnCreatePhysicsState();
		}
	}
	else if (PropName == "bUseVirtualParticles")
	{
		if (ClothHandle)
		{
			if (bUseVirtualParticles && !SimIndices.IsEmpty())
			{
				ClothBridge_SetVirtualParticles(ClothHandle, SimIndices.GetData(), static_cast<uint32>(SimIndices.Num() / 3));
			}
			else
			{
				ClothBridge_SetVirtualParticles(ClothHandle, nullptr, 0);
			}
		}
	}
	else if (PropName == "bEnableClothSimulation")
	{
		if (bEnableClothSimulation && !ClothHandle && IsRegistered())
		{
			OnCreatePhysicsState();
		}
		else if (!bEnableClothSimulation && ClothHandle)
		{
			OnDestroyPhysicsState();
		}
	}
	else
	{
		bParamsDirty = true;
	}
}

bool UClothComponent::ShouldCreatePhysicsState() const
{
	return bEnableClothSimulation && !RestPositions.IsEmpty();
}

void UClothComponent::OnRegister()
{
	if (RestPositions.IsEmpty())
	{
		RebuildClothMesh();
	}
	Super::OnRegister();
}

void UClothComponent::OnCreatePhysicsState()
{
	Super::OnCreatePhysicsState();
	CreateNvClothInstance();
	if (ClothHandle && GetWorld())
	{
		if (FPhysScene* PhysScene = GetWorld()->GetPhysicsScene())
		{
			PhysScene->RegisterClothComponent(this);
		}
	}
}

void UClothComponent::OnDestroyPhysicsState()
{
	if (GetWorld())
	{
		if (FPhysScene* PhysScene = GetWorld()->GetPhysicsScene())
		{
			PhysScene->UnregisterClothComponent(this);
		}
	}
	DestroyNvClothInstance();
	Super::OnDestroyPhysicsState();
}

void UClothComponent::RebuildClothMesh()
{
	RestPositions.Reset();
	RestNormals.Reset();
	RestUVs.Reset();
	InvMasses.Reset();
	SimIndices.Reset();
	RenderVertices.Reset();
	RenderIndices.Reset();

	if (Preset == EClothPreset::StaticMesh)
	{
		if (!BuildStaticMeshData())
		{
			BuildGridData();
		}
	}
	else if (Preset == EClothPreset::Cone)
	{
		BuildConeData();
	}
	else
	{
		BuildGridData();
	}

	const uint32 SimVertexCount = static_cast<uint32>(RestPositions.Num());
	const uint32 FrontVertexCount = SimVertexCount;
	const uint32 TotalVertexCount = bDoubleSidedMesh ? (FrontVertexCount * 2u) : FrontVertexCount;
	RenderVertices.SetNum(TotalVertexCount);
	for (uint32 i = 0; i < FrontVertexCount; ++i)
	{
		RenderVertices[i].Position = RestPositions[i];
		RenderVertices[i].UV = RestUVs[i];
		RenderVertices[i].Color = FVector4(1.0f, 1.0f, 1.0f, 1.0f);
		if (bDoubleSidedMesh)
		{
			RenderVertices[FrontVertexCount + i] = RenderVertices[i];
		}
	}

	const int32 SimIndexCount = SimIndices.Num();
	if (bDoubleSidedMesh)
	{
		RenderIndices.SetNum(SimIndexCount * 2);
		for (int32 i = 0; i < SimIndexCount; ++i)
		{
			RenderIndices[i] = SimIndices[i];
		}
		for (int32 i = 0; i + 2 < SimIndexCount; i += 3)
		{
			RenderIndices[SimIndexCount + i] = FrontVertexCount + SimIndices[i];
			RenderIndices[SimIndexCount + i + 1] = FrontVertexCount + SimIndices[i + 2];
			RenderIndices[SimIndexCount + i + 2] = FrontVertexCount + SimIndices[i + 1];
		}
	}
	else
	{
		RenderIndices = SimIndices;
	}

	RecomputeVertexNormals();
	RestNormals.SetNum(SimVertexCount);
	for (uint32 i = 0; i < SimVertexCount; ++i)
	{
		RestNormals[i] = RenderVertices[i].Normal;
	}
	UpdateBoundsFromVertices();
	CachedProxyBounds = LocalBounds;
	CreateGpuBuffers();
	MarkRenderStateDirty();
	OnTransformDirty(ETeleportType::None);
}

void UClothComponent::ResetClothPose()
{
	if (!ClothHandle || RestPositions.IsEmpty())
	{
		return;
	}
	const uint32 Count = static_cast<uint32>(RestPositions.Num());
	ParticleScratch.SetNum(static_cast<int32>(Count));
	for (uint32 i = 0; i < Count; ++i)
	{
		ParticleScratch[i] = { RestPositions[i].X, RestPositions[i].Y, RestPositions[i].Z, InvMasses[i] };
	}
	ClothBridge_WriteParticles(ClothHandle, ParticleScratch.GetData(), Count);
	bTeleportPending = true;
}

void UClothComponent::PreClothSimulate()
{
	if (!ClothHandle || !bEnableClothSimulation)
	{
		return;
	}

	if (TargetSkeletalMesh && TargetSkeletalMesh->GetComponentSpaceTransform().IsEmpty())
	{
		return;
	}

	if (bParamsDirty)
	{
		PushClothParameters();
		bParamsDirty = false;
	}

	const FVector WorldPos = GetWorldLocation();
	const FQuat WorldRot = GetWorldRotation().Quaternion();
	const FClothBridgeVec3 BridgePos = { WorldPos.X, WorldPos.Y, WorldPos.Z };
	const FClothBridgeQuat BridgeRot = { WorldRot.X, WorldRot.Y, WorldRot.Z, WorldRot.W };
	const bool bJustTeleported = bTeleportPending;
	ClothBridge_SetTransform(ClothHandle, &BridgePos, &BridgeRot, bJustTeleported);
	bTeleportPending = false;

	const bool bResetInterpolation = bJustTeleported || !bInitialDrapeDone;
	UpdateBonePins(bResetInterpolation);
	PushBoneColliders();
	if (bResetInterpolation)
	{
		ClothBridge_ClearInterpolation(ClothHandle);
		bInitialDrapeDone = true;
	}
}

void UClothComponent::SetTargetSkeletalMesh(USkeletalMeshComponent* InMeshComponent)
{
	TargetSkeletalMesh = InMeshComponent;
	bInitialDrapeDone = false;
}

void UClothComponent::PinVertexToBone(uint32 VertexIndex, const FString& BoneName)
{
	if (BoneName.empty())
	{
		return;
	}
	FClothBonePin Pin;
	Pin.VertexIndex = VertexIndex;
	Pin.BoneA = BoneName;
	Pin.WeightB = 0.0f;
	BonePins.Add(Pin);
	bInitialDrapeDone = false;
}

void UClothComponent::PinVertexBetweenBones(uint32 VertexIndex, const FString& BoneA, const FString& BoneB, float WeightB)
{
	if (BoneA.empty())
	{
		return;
	}
	FClothBonePin Pin;
	Pin.VertexIndex = VertexIndex;
	Pin.BoneA = BoneA;
	Pin.BoneB = BoneB;
	Pin.WeightB = std::clamp(WeightB, 0.0f, 1.0f);
	BonePins.Add(Pin);
	bInitialDrapeDone = false;
}

void UClothComponent::PinRowToBone(int32 RowIndex, const FString& BoneName)
{
	if (BoneName.empty() || RowIndex < 0)
	{
		return;
	}
	const int32 SegX = (Preset == EClothPreset::Cone) ? std::max(SegmentsX, 4) : std::max(SegmentsX, 1);
	const int32 SegY = std::max(SegmentsY, 1);
	if (RowIndex > SegY)
	{
		return;
	}
	const int32 VertsX = (Preset == EClothPreset::Cone) ? SegX : (SegX + 1);
	const int32 StartIdx = RowIndex * VertsX;
	for (int32 x = 0; x < VertsX; ++x)
	{
		PinVertexToBone(static_cast<uint32>(StartIdx + x), BoneName);
	}
}

void UClothComponent::ClearBonePins()
{
	BonePins.Reset();
	bInitialDrapeDone = false;
}

void UClothComponent::AddBoneCapsuleCollider(const FString& InBoneA, const FString& InBoneB, float InRadiusA, float InRadiusB)
{
	FClothBoneCapsule Entry;
	Entry.BoneA = InBoneA;
	Entry.BoneB = InBoneB;
	Entry.RadiusA = InRadiusA;
	Entry.RadiusB = InRadiusB;
	BoneCapsules.Add(Entry);
}

void UClothComponent::ClearBoneCapsuleColliders()
{
	BoneCapsules.Reset();
	if (ClothHandle)
	{
		ClothBridge_SetSpheresAndCapsules(ClothHandle, nullptr, 0, nullptr, 0);
	}
}

void UClothComponent::PostClothSimulate()
{
	if (!ClothHandle || !bEnableClothSimulation || RestPositions.IsEmpty())
	{
		return;
	}

	const uint32 SimVertexCount = static_cast<uint32>(RestPositions.Num());
	ParticleScratch.SetNum(static_cast<int32>(SimVertexCount));
	ClothBridge_ReadParticles(ClothHandle, ParticleScratch.GetData(), SimVertexCount);

	for (uint32 i = 0; i < SimVertexCount; ++i)
	{
		const FVector SimPos(ParticleScratch[i].X, ParticleScratch[i].Y, ParticleScratch[i].Z);
		RenderVertices[i].Position = SimPos;
		if (bDoubleSidedMesh)
		{
			RenderVertices[SimVertexCount + i].Position = SimPos;
		}
	}

	RecomputeVertexNormals();
	UpdateBoundsFromVertices();
	UpdateDynamicVertexBuffer();

	if (LocalBounds.Min.X < CachedProxyBounds.Min.X || LocalBounds.Min.Y < CachedProxyBounds.Min.Y || LocalBounds.Min.Z < CachedProxyBounds.Min.Z ||
		LocalBounds.Max.X > CachedProxyBounds.Max.X || LocalBounds.Max.Y > CachedProxyBounds.Max.Y || LocalBounds.Max.Z > CachedProxyBounds.Max.Z)
	{
		const FVector Margin = (LocalBounds.Max - LocalBounds.Min) * 0.25f;
		CachedProxyBounds = FBox(LocalBounds.Min - Margin, LocalBounds.Max + Margin);
		OnTransformDirty(ETeleportType::None);
	}
}

void UClothComponent::BuildGridData()
{
	const int32 SegX = std::max(SegmentsX, 1);
	const int32 SegY = std::max(SegmentsY, 1);
	const float TopW = std::max(ClothWidth, 0.05f);
	const float BotW = (BottomWidth > 0.0f) ? std::max(BottomWidth, 0.05f) : TopW;
	const float Height = std::max(ClothHeight, 0.05f);

	const int32 VertsX = SegX + 1;
	const int32 VertsY = SegY + 1;
	RestPositions.Reserve(VertsX * VertsY);
	RestUVs.Reserve(VertsX * VertsY);
	InvMasses.Reserve(VertsX * VertsY);

	// 세로 격자를 로컬 평면에 생성
	for (int32 y = 0; y < VertsY; ++y)
	{
		const float V = static_cast<float>(y) / static_cast<float>(SegY);
		const float RowWidth = TopW + (BotW - TopW) * V;
		const float PosZ = -V * Height;
		const bool bPinned = bPinTopRow && (y == 0);
		for (int32 x = 0; x < VertsX; ++x)
		{
			const float U = static_cast<float>(x) / static_cast<float>(SegX);
			const float PosY = (U - 0.5f) * RowWidth;
			RestPositions.Add(FVector(0.0f, PosY, PosZ));
			RestUVs.Add(FVector2(U, V));
			InvMasses.Add(bPinned ? 0.0f : 1.0f);
		}
	}

	SimIndices.Reserve(SegX * SegY * 6);
	for (int32 y = 0; y < SegY; ++y)
	{
		for (int32 x = 0; x < SegX; ++x)
		{
			const uint32 I0 = static_cast<uint32>(y * VertsX + x);
			const uint32 I1 = I0 + 1u;
			const uint32 I2 = I0 + static_cast<uint32>(VertsX);
			const uint32 I3 = I2 + 1u;

			SimIndices.Add(I0);
			SimIndices.Add(I1);
			SimIndices.Add(I2);

			SimIndices.Add(I1);
			SimIndices.Add(I3);
			SimIndices.Add(I2);
		}
	}
}

void UClothComponent::BuildConeData()
{
	const int32 SegX = std::max(SegmentsX, 4);
	const int32 SegY = std::max(SegmentsY, 1);
	const float RTop = std::max(TopRadius, 0.02f);
	const float RBot = std::max(BottomRadius, 0.02f);
	const float Height = std::max(ClothHeight, 0.05f);
	constexpr float TwoPi = 6.28318530718f;

	const int32 VertsX = SegX;
	const int32 VertsY = SegY + 1;
	RestPositions.Reserve(VertsX * VertsY);
	RestUVs.Reserve(VertsX * VertsY);
	InvMasses.Reserve(VertsX * VertsY);

	for (int32 y = 0; y < VertsY; ++y)
	{
		const float V = static_cast<float>(y) / static_cast<float>(SegY);
		const float Radius = RTop + (RBot - RTop) * V;
		const float PosZ = -V * Height;
		const bool bPinned = bPinTopRow && (y == 0);
		for (int32 x = 0; x < VertsX; ++x)
		{
			const float U = static_cast<float>(x) / static_cast<float>(SegX);
			const float Angle = U * TwoPi;
			const float PosX = std::cos(Angle) * Radius;
			const float PosY = std::sin(Angle) * Radius;
			RestPositions.Add(FVector(PosX, PosY, PosZ));
			RestUVs.Add(FVector2(U, V));
			InvMasses.Add(bPinned ? 0.0f : 1.0f);
		}
	}

	SimIndices.Reserve(SegX * SegY * 6);
	for (int32 y = 0; y < SegY; ++y)
	{
		for (int32 x = 0; x < SegX; ++x)
		{
			const int32 NextX = (x + 1) % SegX;
			const uint32 I0 = static_cast<uint32>(y * VertsX + x);
			const uint32 I1 = static_cast<uint32>(y * VertsX + NextX);
			const uint32 I2 = static_cast<uint32>((y + 1) * VertsX + x);
			const uint32 I3 = static_cast<uint32>((y + 1) * VertsX + NextX);

			SimIndices.Add(I0);
			SimIndices.Add(I2);
			SimIndices.Add(I1);

			SimIndices.Add(I1);
			SimIndices.Add(I2);
			SimIndices.Add(I3);
		}
	}
}

bool UClothComponent::BuildStaticMeshData()
{
	if (!SourceStaticMesh)
	{
		return false;
	}
	const FStaticMeshData& MeshData = SourceStaticMesh->GetMeshData();
	if (MeshData.Vertices.IsEmpty() || MeshData.Indices.Num() < 3)
	{
		return false;
	}

	float MinZ = FLT_MAX;
	float MaxZ = -FLT_MAX;
	const int32 VertexCount = MeshData.Vertices.Num();
	RestPositions.SetNum(VertexCount);
	RestUVs.SetNum(VertexCount);
	InvMasses.SetNum(VertexCount);

	for (int32 i = 0; i < VertexCount; ++i)
	{
		const FVertex& V = MeshData.Vertices[i];
		RestPositions[i] = V.Position;
		RestUVs[i] = V.UV;
		MinZ = FMath::Min(MinZ, V.Position.Z);
		MaxZ = FMath::Max(MaxZ, V.Position.Z);
	}

	const float HeightSpan = std::max(MaxZ - MinZ, 0.0001f);
	const float PinThresholdZ = MinZ + HeightSpan * std::clamp(PinHeightRatio, 0.0f, 1.0f);
	for (int32 i = 0; i < VertexCount; ++i)
	{
		const bool bPinned = bPinTopRow && (RestPositions[i].Z >= PinThresholdZ);
		InvMasses[i] = bPinned ? 0.0f : 1.0f;
	}

	SimIndices = MeshData.Indices;
	return true;
}

void UClothComponent::CreateGpuBuffers()
{
	DynamicVertexBuffer.reset();
	IndexBuffer.reset();

	if (RenderVertices.IsEmpty() || RenderIndices.IsEmpty())
	{
		return;
	}

	const uint32 VertexBufferBytes = sizeof(FVertex) * static_cast<uint32>(RenderVertices.Num());
	DynamicVertexBuffer = RenderCommand::CreateDynamicVertexBuffer(VertexBufferBytes, sizeof(FVertex));
	IndexBuffer = RenderCommand::CreateStaticIndexBuffer(RenderIndices.GetData(), static_cast<uint32>(RenderIndices.Num()));

	if (DynamicVertexBuffer && IndexBuffer)
	{
		VertexFactory.Init(DynamicVertexBuffer.get(), IndexBuffer.get());
		UpdateDynamicVertexBuffer();
	}
}

void UClothComponent::UpdateDynamicVertexBuffer()
{
	if (!DynamicVertexBuffer || RenderVertices.IsEmpty())
	{
		return;
	}
	void* Dst = RenderCommand::MapWriteDiscard(DynamicVertexBuffer.get());
	if (!Dst)
	{
		return;
	}
	std::memcpy(Dst, RenderVertices.GetData(), sizeof(FVertex) * static_cast<size_t>(RenderVertices.Num()));
	RenderCommand::Unmap(DynamicVertexBuffer.get());
}

void UClothComponent::RecomputeVertexNormals()
{
	const int32 SimVertexCount = RestPositions.Num();
	if (SimVertexCount == 0 || SimIndices.Num() < 3)
	{
		return;
	}

	for (int32 i = 0; i < SimVertexCount; ++i)
	{
		RenderVertices[i].Normal = FVector(0.0f, 0.0f, 0.0f);
	}

	const int32 SimIndexCount = SimIndices.Num();
	for (int32 i = 0; i + 2 < SimIndexCount; i += 3)
	{
		const uint32 I0 = SimIndices[i];
		const uint32 I1 = SimIndices[i + 1];
		const uint32 I2 = SimIndices[i + 2];

		const FVector& P0 = RenderVertices[I0].Position;
		const FVector& P1 = RenderVertices[I1].Position;
		const FVector& P2 = RenderVertices[I2].Position;
		const FVector FaceNormal = FVector::Cross(P1 - P0, P2 - P0);

		RenderVertices[I0].Normal += FaceNormal;
		RenderVertices[I1].Normal += FaceNormal;
		RenderVertices[I2].Normal += FaceNormal;
	}

	for (int32 i = 0; i < SimVertexCount; ++i)
	{
		FVector N = RenderVertices[i].Normal;
		const float LenSq = N.X * N.X + N.Y * N.Y + N.Z * N.Z;
		N = (LenSq > 1e-12f) ? N.Normalized() : FVector(1.0f, 0.0f, 0.0f);
		RenderVertices[i].Normal = N;

		const FVector RefAxis = (std::fabs(N.Z) < 0.9f) ? FVector(0.0f, 0.0f, 1.0f) : FVector(1.0f, 0.0f, 0.0f);
		const FVector Tangent = FVector::Cross(RefAxis, N).Normalized();
		RenderVertices[i].Tangent = FVector4(Tangent.X, Tangent.Y, Tangent.Z, 1.0f);

		if (bDoubleSidedMesh)
		{
			const int32 BackIndex = SimVertexCount + i;
			RenderVertices[BackIndex].Normal = N * -1.0f;
			RenderVertices[BackIndex].Tangent = FVector4(Tangent.X, Tangent.Y, Tangent.Z, -1.0f);
		}
	}
}

void UClothComponent::UpdateBoundsFromVertices()
{
	if (RenderVertices.IsEmpty())
	{
		LocalBounds = FBox(FVector(-0.1f, -0.1f, -0.1f), FVector(0.1f, 0.1f, 0.1f));
		return;
	}

	FVector MinP(FLT_MAX, FLT_MAX, FLT_MAX);
	FVector MaxP(-FLT_MAX, -FLT_MAX, -FLT_MAX);
	const int32 SimVertexCount = RestPositions.Num();
	for (int32 i = 0; i < SimVertexCount; ++i)
	{
		const FVector& P = RenderVertices[i].Position;
		MinP.X = FMath::Min(MinP.X, P.X);
		MinP.Y = FMath::Min(MinP.Y, P.Y);
		MinP.Z = FMath::Min(MinP.Z, P.Z);
		MaxP.X = FMath::Max(MaxP.X, P.X);
		MaxP.Y = FMath::Max(MaxP.Y, P.Y);
		MaxP.Z = FMath::Max(MaxP.Z, P.Z);
	}
	LocalBounds = FBox(MinP, MaxP);
}

void UClothComponent::CreateNvClothInstance()
{
	DestroyNvClothInstance();

	FClothFactoryHandle Factory = FClothCore::GetFactory();
	if (!Factory || RestPositions.IsEmpty() || SimIndices.Num() < 3)
	{
		return;
	}

	FClothBridgeMeshDesc MeshDesc;
	MeshDesc.Positions = &RestPositions[0].X;
	MeshDesc.PositionStride = sizeof(FVector);
	MeshDesc.VertexCount = static_cast<uint32>(RestPositions.Num());
	MeshDesc.InvMasses = InvMasses.GetData();
	MeshDesc.InvMassStride = sizeof(float);
	MeshDesc.Indices = SimIndices.GetData();
	MeshDesc.TriangleCount = static_cast<uint32>(SimIndices.Num() / 3);
	MeshDesc.CookGravity = { 0.0f, 0.0f, -1.0f };
	MeshDesc.bUseGeodesicTether = true;

	FabricHandle = ClothBridge_CookFabric(Factory, &MeshDesc);
	if (!FabricHandle)
	{
		KRS_LOG(Warning, "[NvCloth] Failed to cook fabric");
		return;
	}

	const uint32 Count = static_cast<uint32>(RestPositions.Num());
	ParticleScratch.SetNum(static_cast<int32>(Count));
	for (uint32 i = 0; i < Count; ++i)
	{
		ParticleScratch[i] = { RestPositions[i].X, RestPositions[i].Y, RestPositions[i].Z, InvMasses[i] };
	}

	ClothHandle = ClothBridge_CreateCloth(Factory, FabricHandle, ParticleScratch.GetData(), Count);
	if (!ClothHandle)
	{
		KRS_LOG(Warning, "[NvCloth] Failed to create cloth instance");
		ClothBridge_DestroyFabric(FabricHandle);
		FabricHandle = nullptr;
		return;
	}

	bParamsDirty = true;
	bTeleportPending = true;
	bInitialDrapeDone = false;
	if (bUseVirtualParticles)
	{
		ClothBridge_SetVirtualParticles(ClothHandle, SimIndices.GetData(), static_cast<uint32>(SimIndices.Num() / 3));
	}
	PushClothParameters();
}

void UClothComponent::DestroyNvClothInstance()
{
	if (ClothHandle)
	{
		ClothBridge_DestroyCloth(ClothHandle);
		ClothHandle = nullptr;
	}
	if (FabricHandle)
	{
		ClothBridge_DestroyFabric(FabricHandle);
		FabricHandle = nullptr;
	}
}

void UClothComponent::PushClothParameters()
{
	if (!ClothHandle)
	{
		return;
	}

	FClothBridgeParams Params;
	Params.Gravity = { Gravity.X, Gravity.Y, Gravity.Z };
	Params.Damping = { Damping.X, Damping.Y, Damping.Z };
	Params.LinearInertia = { LinearInertiaScale, LinearInertiaScale, LinearInertiaScale };
	Params.AngularInertia = { AngularInertiaScale, AngularInertiaScale, AngularInertiaScale };
	Params.CentrifugalInertia = { CentrifugalInertiaScale, CentrifugalInertiaScale, CentrifugalInertiaScale };
	Params.WindVelocity = { WindVelocity.X, WindVelocity.Y, WindVelocity.Z };
	Params.DragCoefficient = DragCoefficient;
	Params.LiftCoefficient = LiftCoefficient;
	Params.SolverFrequency = std::max(SolverFrequency, 30.0f);
	Params.StretchStiffness = std::clamp(StretchStiffness, 0.0f, 1.0f);
	Params.BendStiffness = std::clamp(BendStiffness, 0.0f, 1.0f);
	Params.ShearStiffness = std::clamp(ShearStiffness, 0.0f, 1.0f);
	Params.TetherScale = std::max(TetherScale, 0.0f);
	Params.TetherStiffness = std::clamp(TetherStiffness, 0.0f, 1.0f);
	Params.bEnableContinuousCollision = bEnableContinuousCollision;
	ClothBridge_ApplyParams(ClothHandle, FabricHandle, &Params);
}

void UClothComponent::PushBoneColliders()
{
	if (!ClothHandle)
	{
		return;
	}

	if (!TargetSkeletalMesh || !TargetSkeletalMesh->GetSkeletalMesh() || BoneCapsules.IsEmpty() || TargetSkeletalMesh->GetComponentSpaceTransform().IsEmpty())
	{
		return;
	}

	SphereScratch.Reset();
	CapsuleIndexScratch.Reset();

	const FMatrix WorldToCloth = GetWorldMatrix().Inverse();
	for (int32 i = 0; i < BoneCapsules.Num(); ++i)
	{
		const FClothBoneCapsule& Cap = BoneCapsules[i];
		const int32 IdxA = TargetSkeletalMesh->FindBoneIndex(Cap.BoneA);
		const int32 IdxB = TargetSkeletalMesh->FindBoneIndex(Cap.BoneB);
		if (IdxA < 0 || IdxB < 0)
		{
			continue;
		}

		const FVector WorldA = TargetSkeletalMesh->GetBoneWorldMatrix(IdxA).TransformPosition(FVector::ZeroVector);
		const FVector WorldB = TargetSkeletalMesh->GetBoneWorldMatrix(IdxB).TransformPosition(FVector::ZeroVector);
		const FVector LocalA = WorldToCloth.TransformPosition(WorldA);
		const FVector LocalB = WorldToCloth.TransformPosition(WorldB);

		const uint32 SphereIdxA = static_cast<uint32>(SphereScratch.Num());
		SphereScratch.Add({ LocalA.X, LocalA.Y, LocalA.Z, std::max(Cap.RadiusA, 0.01f) });
		const uint32 SphereIdxB = static_cast<uint32>(SphereScratch.Num());
		SphereScratch.Add({ LocalB.X, LocalB.Y, LocalB.Z, std::max(Cap.RadiusB, 0.01f) });

		CapsuleIndexScratch.Add(SphereIdxA);
		CapsuleIndexScratch.Add(SphereIdxB);
	}

	ClothBridge_SetSpheresAndCapsules(
		ClothHandle,
		SphereScratch.IsEmpty() ? nullptr : SphereScratch.GetData(),
		static_cast<uint32>(SphereScratch.Num()),
		CapsuleIndexScratch.IsEmpty() ? nullptr : CapsuleIndexScratch.GetData(),
		static_cast<uint32>(CapsuleIndexScratch.Num()));
}

FMatrix UClothComponent::ComputeBoneSkinMatrix(int32 BoneIndex) const
{
	if (!TargetSkeletalMesh || !TargetSkeletalMesh->GetSkeletalMesh())
	{
		return FMatrix::Identity;
	}
	const TArray<FBoneInfo>& Bones = TargetSkeletalMesh->GetSkeletalMesh()->GetRefSkeleton();
	const TArray<FMatrix>& CompTransforms = TargetSkeletalMesh->GetComponentSpaceTransform();
	if (BoneIndex < 0 || BoneIndex >= Bones.Num() || BoneIndex >= CompTransforms.Num())
	{
		return FMatrix::Identity;
	}
	return Bones[BoneIndex].InverseBindPose * CompTransforms[BoneIndex];
}

void UClothComponent::UpdateBonePins(bool bInitializeFullMesh)
{
	if (!ClothHandle || !TargetSkeletalMesh || !TargetSkeletalMesh->GetSkeletalMesh() || TargetSkeletalMesh->GetComponentSpaceTransform().IsEmpty())
	{
		return;
	}

	if (BonePins.IsEmpty())
	{
		ClothBridge_SetSeparationConstraints(ClothHandle, nullptr, 0);
		ClothBridge_SetMotionConstraints(ClothHandle, nullptr, 0);
		return;
	}

	const FMatrix ClothWorld = GetWorldMatrix();
	const FMatrix MeshWorld = TargetSkeletalMesh->GetWorldMatrix();
	const FMatrix ClothToMesh = ClothWorld * MeshWorld.Inverse();
	const FMatrix MeshToCloth = MeshWorld * ClothWorld.Inverse();
	const int32 AnchorBoneIdx = TargetSkeletalMesh->FindBoneIndex(BonePins[0].BoneA);
	const FMatrix AnchorSkinM = (AnchorBoneIdx >= 0) ? ComputeBoneSkinMatrix(AnchorBoneIdx) : FMatrix::Identity;

	if (bInitializeFullMesh && AnchorBoneIdx >= 0)
	{
		const int32 Count = RestPositions.Num();
		ParticleScratch.SetNum(Count);
		for (int32 i = 0; i < Count; ++i)
		{
			const FVector MeshBindPos = ClothToMesh.TransformPosition(RestPositions[i]);
			const FVector AnimClothPos = MeshToCloth.TransformPosition(AnchorSkinM.TransformPosition(MeshBindPos));
			ParticleScratch[i] = { AnimClothPos.X, AnimClothPos.Y, AnimClothPos.Z, InvMasses[i] };
		}
		ClothBridge_WriteParticles(ClothHandle, ParticleScratch.GetData(), static_cast<uint32>(Count));
	}

	const int32 RestCount = RestPositions.Num();
	if (bUseBackstop && RestNormals.Num() == RestCount && BackstopRadius > 0.0f)
	{
		ConstraintScratch.SetNum(RestCount);
		const float SphereShift = BackstopOffset + BackstopRadius;
		for (int32 i = 0; i < RestCount; ++i)
		{
			const FVector MeshBindPos = ClothToMesh.TransformPosition(RestPositions[i]);
			const FVector MeshBindNormal = ClothToMesh.TransformVector(RestNormals[i]).GetSafeNormal();
			const FVector AnimPos = MeshToCloth.TransformPosition(AnchorSkinM.TransformPosition(MeshBindPos));
			const FVector AnimNormal = MeshToCloth.TransformVector(AnchorSkinM.TransformVector(MeshBindNormal)).GetSafeNormal();
			const FVector SphereCenter = AnimPos - AnimNormal * SphereShift;
			ConstraintScratch[i] = { SphereCenter.X, SphereCenter.Y, SphereCenter.Z, BackstopRadius };
		}
		ClothBridge_SetSeparationConstraints(ClothHandle, ConstraintScratch.GetData(), static_cast<uint32>(RestCount));
	}
	else
	{
		ClothBridge_SetSeparationConstraints(ClothHandle, nullptr, 0);
	}

	if (bUseMotionConstraints && MaxMotionDistance > 0.0f && RestUVs.Num() == RestCount)
	{
		ConstraintScratch.SetNum(RestCount);
		for (int32 i = 0; i < RestCount; ++i)
		{
			const FVector MeshBindPos = ClothToMesh.TransformPosition(RestPositions[i]);
			const FVector AnimPos = MeshToCloth.TransformPosition(AnchorSkinM.TransformPosition(MeshBindPos));
			const float AllowedRadius = (InvMasses[i] <= 0.0f) ? 0.0f : (MaxMotionDistance * std::clamp(RestUVs[i].Y, 0.0f, 1.0f));
			ConstraintScratch[i] = { AnimPos.X, AnimPos.Y, AnimPos.Z, AllowedRadius };
		}
		ClothBridge_SetMotionConstraints(ClothHandle, ConstraintScratch.GetData(), static_cast<uint32>(RestCount));
	}
	else
	{
		ClothBridge_SetMotionConstraints(ClothHandle, nullptr, 0);
	}

	if (bInitializeFullMesh && AnchorBoneIdx >= 0)
	{
		ClothBridge_ClearInterpolation(ClothHandle);
		bInitialDrapeDone = true;
	}

	PinnedVertexIndices.Reset();
	PinnedLocalScratch.Reset();
	PinnedVertexIndices.Reserve(BonePins.Num());
	PinnedLocalScratch.Reserve(BonePins.Num());

	for (int32 i = 0; i < BonePins.Num(); ++i)
	{
		const FClothBonePin& Pin = BonePins[i];
		if (static_cast<int32>(Pin.VertexIndex) >= RestCount)
		{
			continue;
		}
		const int32 IdxA = TargetSkeletalMesh->FindBoneIndex(Pin.BoneA);
		if (IdxA < 0)
		{
			continue;
		}

		const FVector MeshBindPos = ClothToMesh.TransformPosition(RestPositions[Pin.VertexIndex]);
		FVector AnimMeshPos = ComputeBoneSkinMatrix(IdxA).TransformPosition(MeshBindPos);
		if (!Pin.BoneB.empty() && Pin.WeightB > 0.0f)
		{
			const int32 IdxB = TargetSkeletalMesh->FindBoneIndex(Pin.BoneB);
			if (IdxB >= 0)
			{
				const FVector AnimMeshPosB = ComputeBoneSkinMatrix(IdxB).TransformPosition(MeshBindPos);
				AnimMeshPos = AnimMeshPos + (AnimMeshPosB - AnimMeshPos) * Pin.WeightB;
			}
		}

		const FVector ClothLocalPos = MeshToCloth.TransformPosition(AnimMeshPos);
		PinnedVertexIndices.Add(Pin.VertexIndex);
		PinnedLocalScratch.Add({ ClothLocalPos.X, ClothLocalPos.Y, ClothLocalPos.Z });
	}

	if (!PinnedVertexIndices.IsEmpty())
	{
		ClothBridge_UpdatePinnedParticles(
			ClothHandle,
			PinnedVertexIndices.GetData(),
			PinnedLocalScratch.GetData(),
			static_cast<uint32>(PinnedVertexIndices.Num()));
	}
}

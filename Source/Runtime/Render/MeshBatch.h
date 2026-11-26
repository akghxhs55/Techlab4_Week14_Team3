#pragma once

#include "Core/Types.h"
#include "Containers/Array.h"
#include "Math/Transform.h"
#include "Mesh.h"
#include "SkeletalMesh.h"
#include "Shader.h"
#include "Material.h"
#include "Render/VertexFactory.h"
#include "Render/Buffer.h"
#include "Render/RenderCommand.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <utility>

class FPrimitiveSceneProxy;
inline constexpr uint32 InvalidPrimitiveId = ~0u;

class FGlobalDynamicVertexBuffer
{
public:
	struct FAllocation
	{
		uint8* Data = nullptr;
		int32 BaseVertex = 0;
	};

	void Reset(uint32 InStride)
	{
		Stride = InStride;
		StagingData.Reset();
	}

	FAllocation Allocate(uint32 VertexCount)
	{
		FAllocation Result{};
		if (VertexCount == 0 || Stride == 0)
			return Result;

		const uint32 CurrentBytes = static_cast<uint32>(StagingData.Num());
		const uint32 AddBytes = VertexCount * Stride;
		StagingData.SetNum(CurrentBytes + AddBytes);
		Result.Data = StagingData.GetData() + CurrentBytes;
		Result.BaseVertex = static_cast<int32>(CurrentBytes / Stride);
		return Result;
	}

	FVertexBuffer* Commit()
	{
		if (StagingData.IsEmpty() || Stride == 0)
			return GpuBuffer.get();

		const uint32 RequiredBytes = static_cast<uint32>(StagingData.Num());
		if (!GpuBuffer || CapacityBytes < RequiredBytes || GpuBuffer->GetStride() != Stride)
		{
			uint32 NewCapacity = std::max(CapacityBytes * 2u, Stride * 256u);
			while (NewCapacity < RequiredBytes)
				NewCapacity *= 2u;
			GpuBuffer = RenderCommand::CreateDynamicVertexBuffer(NewCapacity, Stride);
			CapacityBytes = GpuBuffer ? NewCapacity : 0;
		}
		if (GpuBuffer)
		{
			if (void* Mapped = RenderCommand::MapWriteDiscard(GpuBuffer.get()))
			{
				std::memcpy(Mapped, StagingData.GetData(), RequiredBytes);
				RenderCommand::Unmap(GpuBuffer.get());
			}
		}
		return GpuBuffer.get();
	}

	FVertexBuffer* GetBuffer() const { return GpuBuffer.get(); }

private:
	TArray<uint8> StagingData;
	TUniquePtr<FVertexBuffer> GpuBuffer;
	uint32 CapacityBytes = 0;
	uint32 Stride = 0;
};

class FGlobalDynamicIndexBuffer
{
public:
	struct FAllocation
	{
		uint32* Data = nullptr;
		uint32 FirstIndex = 0;
	};

	void Reset()
	{
		StagingData.Reset();
	}

	FAllocation Allocate(uint32 IndexCount)
	{
		FAllocation Result{};
		if (IndexCount == 0)
			return Result;

		const uint32 CurrentCount = static_cast<uint32>(StagingData.Num());
		StagingData.SetNum(CurrentCount + IndexCount);
		Result.Data = StagingData.GetData() + CurrentCount;
		Result.FirstIndex = CurrentCount;
		return Result;
	}

	FIndexBuffer* Commit()
	{
		if (StagingData.IsEmpty())
			return GpuBuffer.get();

		const uint32 RequiredCount = static_cast<uint32>(StagingData.Num());
		if (!GpuBuffer || CapacityIndices < RequiredCount)
		{
			uint32 NewCapacity = std::max(CapacityIndices * 2u, 512u);
			while (NewCapacity < RequiredCount)
				NewCapacity *= 2u;
			GpuBuffer = RenderCommand::CreateDynamicIndexBuffer(NewCapacity);
			CapacityIndices = GpuBuffer ? NewCapacity : 0;
		}
		if (GpuBuffer)
		{
			if (void* Mapped = RenderCommand::MapWriteDiscard(GpuBuffer.get()))
			{
				std::memcpy(Mapped, StagingData.GetData(), RequiredCount * sizeof(uint32));
				RenderCommand::Unmap(GpuBuffer.get());
			}
		}
		return GpuBuffer.get();
	}

	FIndexBuffer* GetBuffer() const { return GpuBuffer.get(); }

private:
	TArray<uint32> StagingData;
	TUniquePtr<FIndexBuffer> GpuBuffer;
	uint32 CapacityIndices = 0;
};

struct FMeshBatchElement
{
	const FPrimitiveSceneProxy* Proxy = nullptr;
	FIndexBuffer* IndexBuffer = nullptr;
	const FMatrix* WorldMatrix = nullptr;
	uint32 FirstIndex = 0;
	uint32 IndexCount = 0;
	int32 BaseVertexLocation = 0;
	uint32 InstanceCount = 0;
	uint32 StartInstance = 0;
	uint32 PrimitiveId = InvalidPrimitiveId;
	bool bReceiveShadow = true;
	bool bReceiveDecal = true;
	int32 DebugWeightBone = -1;
};

struct FMeshBatch
{
	const FVertexFactory* VertexFactory = nullptr;
	UStaticMesh* Mesh = nullptr;
	UMaterialInterface* Material = nullptr;
	const void* MaterialParamData = nullptr;
	uint32 MaterialParamDataSize = 0;
	EPSOType OverridePSOType = EPSOType::Count;
	float CameraDistanceSq = 0.0f;
	uint16 MeshSortID = 0;
	uint8 LODIndex = 0;
	bool bIsParticle = false;
	float ParticleAlpha = 1.0f;
	bool bCastShadow = true;
	bool bUsesGPUScene = true;
	bool bTwoSided = false;
	TArray<FMeshBatchElement> Elements;

	const FVertexFactory* ResolveVertexFactory() const
	{
		if (VertexFactory)
			return VertexFactory;
		return Mesh ? Mesh->GetVertexFactory() : nullptr;
	}

	bool IsOpaque() const
	{
		if (OverridePSOType != EPSOType::Count &&
			OverridePSOType != EPSOType::StaticMesh_Opaque &&
			OverridePSOType != EPSOType::StaticMesh_Wireframe)
		{
			return false;
		}
		return Material && Material->GetPSOType() == EPSOType::StaticMesh_Opaque;
	}
};

// 씬 등록 시 생성되는 정적 메시 배치
struct FStaticMeshBatch : public FMeshBatch
{
	const FPrimitiveSceneProxy* PrimitiveSceneProxy = nullptr;
	uint32 PrimitiveIndex = InvalidPrimitiveId;
};

// 정적 프록시 수집 인터페이스
class FStaticPrimitiveDrawInterface
{
public:
	virtual ~FStaticPrimitiveDrawInterface() = default;
	virtual void DrawMesh(const FStaticMeshBatch& Batch) = 0;
};

struct FViewContext
{
	int32 ViewIndex = 0;
	uint32 Width = 0;
	uint32 Height = 0;
	FVector CameraPosition = FVector::ZeroVector;
	FVector CameraForward = FVector::ZeroVector;
	float ProjectionScaleSquared = 0.0f;
	float NearZ = 0.0f;
	bool bOrthographic = false;
	bool bGameView = false;
	bool bShowSkeletalMesh = true;
	bool bShowParticles = true;
	float CameraDepth = 0.0f;
	FMatrix ViewProjection = FMatrix::Identity;

	void Prepare()
	{
		CameraDepth = CameraPosition.X * CameraForward.X
			+ CameraPosition.Y * CameraForward.Y + CameraPosition.Z * CameraForward.Z;
	}

	FMatrix BuildBillboardMatrix(const FVector& WorldPosition, float Width, float Height) const
	{
		// 카메라 화면에 나란하게 세운다
		FVector Forward = CameraForward.Normalized();
		if (Forward.Length() <= 1e-6f)
			Forward = (WorldPosition - CameraPosition).Normalized();
		FVector Right = FVector::Cross(FVector(0.0f, 0.0f, 1.0f), Forward).Normalized();
		if (Right.Length() <= 1e-6f)
			Right = FVector(0.0f, 1.0f, 0.0f);
		const FVector Up = FVector::Cross(Forward, Right).Normalized();
		const FVector Facing = Forward * -1.0f;
		FMatrix Matrix = FMatrix::Identity;
		Matrix.M[0][0] = Facing.X;  Matrix.M[0][1] = Facing.Y;  Matrix.M[0][2] = Facing.Z;
		Matrix.M[1][0] = Right.X * Width;  Matrix.M[1][1] = Right.Y * Width;  Matrix.M[1][2] = Right.Z * Width;
		Matrix.M[2][0] = Up.X * Height; Matrix.M[2][1] = Up.Y * Height; Matrix.M[2][2] = Up.Z * Height;
		Matrix.M[3][0] = WorldPosition.X; Matrix.M[3][1] = WorldPosition.Y; Matrix.M[3][2] = WorldPosition.Z;
		return Matrix;
	}
};

struct FMeshBatch;

struct FGpuParticleState
{
	FVector Position = FVector::ZeroVector;
	float RelativeTime = 0.0f;
	FVector Velocity = FVector::ZeroVector;
	float Lifetime = 1.0f;
	FVector BaseSize = FVector::OneVector;
	float Rotation = 0.0f;
	FVector4 Color = FVector4(1.0f, 1.0f, 1.0f, 1.0f);
	FVector Size = FVector::OneVector;
	float RotationRate = 0.0f;
	FVector4 BaseColor = FVector4(1.0f, 1.0f, 1.0f, 1.0f);
	FVector BaseVelocity = FVector::ZeroVector;
	float BaseRotationRate = 0.0f;
	float SubImageIndex = 0.0f;
	uint32 bAlive = 0;
	uint32 CollisionCount = 0;
	float Pad = 0.0f;
	FVector4 MeshRotation = FVector4(0.0f, 0.0f, 0.0f, 1.0f);
	FVector AngularVelocity = FVector::ZeroVector;
	float SimPad = 0.0f;
};
static_assert(sizeof(FGpuParticleState) == 160, "FGpuParticleState layout mismatch");

struct FGpuParticleEmitterResources
{
	TUniquePtr<FStructuredBuffer> ParticleBuffer;
	TUniquePtr<FStructuredBuffer> SpawnUploadBuffer;
	TUniquePtr<FConstantBuffer> SimCB;
	TUniquePtr<FConstantBuffer> DrawCB;
	uint32 MaxParticles = 0;
	uint32 SpawnCapacity = 0;
};

struct FGpuDistributionDesc
{
	FVector4 AtlasUVAndMode = FVector4(0.0f, 0.0f, 0.0f, 0.0f);
	FVector4 ValueScale = FVector4(1.0f, 1.0f, 1.0f, 1.0f);
	FVector4 ValueBias = FVector4(0.0f, 0.0f, 0.0f, 0.0f);
};
static_assert(sizeof(FGpuDistributionDesc) == 48);

struct FGpuParticleSimParams
{
	float DeltaTime = 0.0f;
	uint32 MaxParticles = 0;
	uint32 SpawnCount = 0;
	uint32 WriteCursor = 0;

	FGpuDistributionDesc SizeScale{};
	FGpuDistributionDesc ColorOverLife{};
	FGpuDistributionDesc SubUV{};
	FGpuDistributionDesc VelocityScale{};
	FGpuDistributionDesc RotationRateScale{};

	float SubUVCols = 1.0f;
	float SubUVRows = 1.0f;
	float SubUVPlayRate = 1.0f;
	uint32 SubUVLinear = 1;

	uint32 HasColorOverride = 0;
	uint32 Pad[3] = {};
	FVector4 ColorOverride = FVector4(1.0f, 1.0f, 1.0f, 1.0f);

	uint32 HasCollision = 0;
	float CollisionRestitution = 0.0f;
	float CollisionFriction = 0.0f;
	uint32 CollisionMaxCount = 0;
	float CollisionRadius = 0.0f;
	float DepthThickness = 0.0f;
	uint32 CollisionPad[2] = {};

	FMatrix ViewProj = FMatrix::Identity;
	FMatrix InvViewProj = FMatrix::Identity;
	FVector4 ScreenAndClip = FVector4(0.0f, 0.0f, 0.0f, 0.0f);
	FVector Acceleration = FVector::ZeroVector;
	float AccelPad = 0.0f;
};
static_assert(sizeof(FGpuParticleSimParams) == 496);

struct FGpuParticleDrawParams
{
	FMatrix LocalToWorld = FMatrix::Identity;
	FVector4 CameraFacing = FVector4(1.0f, 0.0f, 0.0f, 0.0f);
	FVector4 CameraRight = FVector4(0.0f, 1.0f, 0.0f, 0.0f);
	FVector4 CameraUp = FVector4(0.0f, 0.0f, 1.0f, 0.0f);
	FVector4 WorldScaleAndLocal = FVector4(1.0f, 1.0f, 0.0f, 0.0f);
	FVector4 AtlasParams = FVector4(1.0f, 1.0f, 0.0f, 0.0f);
};
static_assert(sizeof(FGpuParticleDrawParams) == 144);

struct FGpuParticleMaterialConstants
{
	FVector4 MaterialBaseColor = FVector4(1.0f, 1.0f, 1.0f, 1.0f);
	float HasTexture = 0.0f;
	float Pad[3] = {};
};
static_assert(sizeof(FGpuParticleMaterialConstants) == 32);

struct FGpuParticleBatch
{
	TSharedPtr<FGpuParticleEmitterResources> GpuResources;
	UStaticMesh* Mesh = nullptr;
	UMaterialInterface* Material = nullptr;
	uint32 MaxParticles = 0;
	uint32 ParticleStride = sizeof(FGpuParticleState);
	bool bResetRequested = false;
	TArray<uint8> PendingSpawnBytes;
	uint32 SpawnCount = 0;
	FGpuParticleSimParams SimParams{};
	FGpuParticleDrawParams DrawParams{};
};

// 뷰별 동적 메시 배치 및 행렬 수집기
class FMeshElementCollector
{
	static constexpr uint32 MatricesPerBlock = 256;
	using FMatrixBlock = std::array<FMatrix, MatricesPerBlock>;

public:
	FMeshElementCollector() = default;
	FMeshElementCollector(const FMeshElementCollector&) = delete;
	FMeshElementCollector& operator=(const FMeshElementCollector&) = delete;

	void Reset()
	{
		MeshBatches.Reset();
		GpuParticleBatches.Reset();
		DynamicVertexBuffer.Reset(sizeof(FVertex));
		DynamicIndexBuffer.Reset();
		MatrixCount = 0;
	}

	const FMatrix* StoreWorldMatrix(const FMatrix& World)
	{
		const uint32 BlockIndex = MatrixCount / MatricesPerBlock;
		if (BlockIndex >= static_cast<uint32>(MatrixBlocks.Num()))
			MatrixBlocks.Add(MakeUnique<FMatrixBlock>());
		FMatrix& Stored = (*MatrixBlocks[BlockIndex])[MatrixCount % MatricesPerBlock];
		Stored = World;
		++MatrixCount;
		return &Stored;
	}

	void AddMesh(FMeshBatch&& Batch)
	{
		MeshBatches.Add(std::move(Batch));
	}

	void AddGpuParticleBatch(FGpuParticleBatch&& Batch)
	{
		GpuParticleBatches.Add(std::move(Batch));
	}

	const TArray<FGpuParticleBatch>& GetGpuParticleBatches() const { return GpuParticleBatches; }

	FMeshBatch& AddMeshDefaulted()
	{
		return MeshBatches.AddDefaulted_GetRef();
	}

	FGlobalDynamicVertexBuffer::FAllocation AllocateDynamicVertices(uint32 VertexCount)
	{
		return DynamicVertexBuffer.Allocate(VertexCount);
	}

	FGlobalDynamicIndexBuffer::FAllocation AllocateDynamicIndices(uint32 IndexCount)
	{
		return DynamicIndexBuffer.Allocate(IndexCount);
	}

	void CommitGpuResources()
	{
		DynamicVertexBuffer.Commit();
		DynamicIndexBuffer.Commit();
	}

	const TArray<FMeshBatch>& GetMeshBatches() const { return MeshBatches; }
	TArray<FMeshBatch>& GetMeshBatchesMutable() { return MeshBatches; }
	FVertexBuffer* GetDynamicVertexBuffer() const { return DynamicVertexBuffer.GetBuffer(); }
	FIndexBuffer* GetDynamicIndexBuffer() const { return DynamicIndexBuffer.GetBuffer(); }

private:
	TArray<FMeshBatch> MeshBatches;
	TArray<FGpuParticleBatch> GpuParticleBatches;
	TArray<TUniquePtr<FMatrixBlock>> MatrixBlocks;
	uint32 MatrixCount = 0;

	FGlobalDynamicVertexBuffer DynamicVertexBuffer;
	FGlobalDynamicIndexBuffer DynamicIndexBuffer;
};

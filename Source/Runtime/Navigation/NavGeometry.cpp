#include "EnginePCh.h"
#include "NavGeometry.h"

#include "Math/Matrix.h"
#include "Render/StaticMeshData.h"
#include <cmath>
#include <limits>
#include <utility>

#if !defined(ENGINE_SHIPPING)
#include "Components/SceneComponent.h"
#include "Render/GeometryGenerator.h"
#include "UObject/UObjectGlobals.h"
#include <DetourNavMeshQuery.h>
#include <Recast.h>
#include <memory>
#endif

namespace
{
	bool IsFinitePosition(const FVector& P)
	{
		return std::isfinite(P.X) &&
			std::isfinite(P.Y) &&
			std::isfinite(P.Z);
	}
}

namespace Navigation
{
	// Recast 좌표계로 변환
	// Recast 좌표계: X, Y, Z -> X, Z, -Y
	FVector ToRecast(const FVector& P)
	{
		return FVector(P.X, P.Z, -P.Y);
	}

	// Recast 좌표계에서 월드 좌표계로 변환
	// Recast 좌표계: X, Y, Z <- X, Z, -Y
	FVector FromRecast(const FVector& P)
	{
		return FVector(P.X, -P.Z, P.Y);
	}

	FRecastGeometry ConvertStaticMesh(
		const FStaticMeshData& Mesh, const FMatrix& LocalToWorld)
	{
		for (const auto& Row : LocalToWorld.M)
		{
			for (float Entry : Row)
			{
				if (!std::isfinite(Entry))
				{
					KRS_LOG(Warning, "Navigation skipped mesh with a non-finite world matrix");
					return {};
				}
			}
		}

		const float Determinant = LocalToWorld.Determinant();
		if (!std::isfinite(Determinant))
		{
			KRS_LOG(Warning, "Navigation skipped mesh with a non-finite determinant");
			return {};
		}
		const bool bFlipWinding = Determinant < 0.0f;

		const int32 VertexCount = Mesh.Vertices.Num();
		if (VertexCount > std::numeric_limits<int32>::max() / 3)
		{
			KRS_LOG(Warning, "Navigation skipped mesh with too many vertices");
			return {};
		}

		TArray<FVector> Positions;
		TArray<int32> OutputIndices;
		Positions.Reserve(VertexCount);
		OutputIndices.Reserve(VertexCount);

		for (const FVertex& Vertex : Mesh.Vertices)
		{
			Positions.Add(ToRecast(LocalToWorld.TransformPosition(Vertex.Position)));
			OutputIndices.Add(-1);
		}

		FRecastGeometry Result;
		const int32 IndexCount = Mesh.Indices.Num();
		const int32 TrailingIndices = IndexCount % 3;
		int32 BadIndices = 0;
		int32 NonFinite = 0;
		int32 Degenerate = 0;

		for (int32 Offset = 0; Offset < IndexCount - TrailingIndices; Offset += 3)
		{
			uint32 Id[3] = {
				Mesh.Indices[Offset],
				Mesh.Indices[Offset + 1],
				Mesh.Indices[Offset + 2]
			};

			const auto Limit = static_cast<uint32>(VertexCount);
			if (Id[0] >= Limit || Id[1] >= Limit || Id[2] >= Limit)
			{
				++BadIndices;
				continue;
			}

			if (bFlipWinding)
			{
				std::swap(Id[1], Id[2]);
			}

			const FVector& A = Positions[Id[0]];
			const FVector& B = Positions[Id[1]];
			const FVector& C = Positions[Id[2]];

			if (!IsFinitePosition(A) || !IsFinitePosition(B) || !IsFinitePosition(C))
			{
				++NonFinite;
				continue;
			}

			const float NormalLengthSquared = (B - A).Cross(C - A).SizeSquared();
			if (!std::isfinite(NormalLengthSquared))
			{
				++NonFinite;
				continue;
			}
			if (NormalLengthSquared <= 0.0f)
			{
				++Degenerate;
				continue;
			}

			for (uint32 Index : Id)
			{
				int& OutputIndex = OutputIndices[Index];
				if (OutputIndex == -1)
				{
					OutputIndex = Result.Vertices.Num() / 3;
					const FVector& P = Positions[Index];
					Result.Vertices.Add(P.X);
					Result.Vertices.Add(P.Y);
					Result.Vertices.Add(P.Z);
				}
				Result.Triangles.Add(OutputIndex);
			}
		}

		if (BadIndices || NonFinite || Degenerate || TrailingIndices)
		{
			KRS_LOG(Warning,
				"Navigation skipped triangles (bad Indices {}, non-finite {}, degenerate {}), trailing indices {}",
				BadIndices, NonFinite, Degenerate, TrailingIndices);
		}

		return Result;
	}
}

#if !defined(ENGINE_SHIPPING)
bool Navigation::RunNavigationSmokeTest()
{
	rcContext Context(false);
	std::unique_ptr<dtNavMeshQuery, decltype(&dtFreeNavMeshQuery)> Query(
		dtAllocNavMeshQuery(), &dtFreeNavMeshQuery);

	bool bPassed = true;
	const auto Check = [&](bool bOk, const char* Name)
		{
			if (!bOk)
			{
				KRS_LOG(Error, "Navigation smoke test failed: {}", Name);
				bPassed = false;
			}
		};

	Check(Query != nullptr, "Detour query allocation");
	Query.reset();

	const FVector World(1.25f, -7.0f, 3.5f);
	const FVector Recast = ToRecast(World);
	Check((Recast - FVector(1.25f, 3.5f, 7.0f)).SizeSquared() < 1e-8f,
		"axis mapping");
	Check((FromRecast(Recast) - World).SizeSquared() < 1e-8f,
		"coordinate round trip");

	const auto Plane = FGeometryGenerator::CreatePlane(1.0f);
	const auto CheckPlane = [&](const FMatrix& Matrix, const FVector& ExpectedFirst,
		const char* Name)
		{
			const FRecastGeometry Geometry = ConvertStaticMesh(*Plane, Matrix);
			const bool bValid = Geometry.Vertices.Num() == 12 &&
				Geometry.Triangles.Num() == 6;
			Check(bValid, Name);
			if (!bValid)
			{
				return;
			}

			const FVector First(Geometry.Vertices[0], Geometry.Vertices[1], Geometry.Vertices[2]);
			Check((First - ExpectedFirst).SizeSquared() < 1e-8f, Name);

			unsigned char Areas[2] = { RC_NULL_AREA, RC_NULL_AREA };
			rcMarkWalkableTriangles(&Context, 45.0f,
				Geometry.Vertices.GetData(), Geometry.Vertices.Num() / 3,
				Geometry.Triangles.GetData(), Geometry.Triangles.Num() / 3,
				Areas);

			Check(Areas[0] == RC_WALKABLE_AREA && Areas[1] == RC_WALKABLE_AREA, Name);
		};

	CheckPlane(FMatrix::Identity, FVector(-0.5f, 0.0f, -0.5f), "identity plane");

	const std::unique_ptr<USceneComponent> Parent(NewObject<USceneComponent>());
	const std::unique_ptr<USceneComponent> Child(NewObject<USceneComponent>());

	Parent->SetRelativeLocation(FVector(10.0f, 20.0f, 30.0f));
	Parent->SetRelativeScale3D(FVector(2.0f, 3.0f, 1.0f));
	Child->SetRelativeLocation(FVector(1.0f, 2.0f, 3.0f));
	Child->SetupAttachment(Parent.get(), EAttachmentRule::KeepRelative);

	CheckPlane(Child->GetWorldMatrix(), FVector(11.0f, 33.0f, -27.5f),
		"attached plane");

	Parent->SetRelativeScale3D(FVector(-2.0f, 3.0f, 1.0f));
	CheckPlane(Child->GetWorldMatrix(), FVector(9.0f, 33.0f, -27.5f),
		"attached plane with mirrored parent");

	FMatrix InvalidMatrix = FMatrix::Identity;
	InvalidMatrix[3][0] = std::numeric_limits<float>::infinity();
	Check(ConvertStaticMesh(*Plane, InvalidMatrix).Triangles.Num() == 0,
		"non-finite matrix skipped");

	Plane->Indices[0] = static_cast<uint32>(Plane->Vertices.Num());
	Check(ConvertStaticMesh(*Plane, FMatrix::Identity).Triangles.Num() == 3,
		"invalid index skips only triangle");

	if (bPassed)
	{
		KRS_LOG(Info, "Navigation smoke test passed");
	}
	return bPassed;
}
#endif

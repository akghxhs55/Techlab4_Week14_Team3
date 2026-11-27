#include "EnginePCH.h"
#include "Navigation/RecastNavMesh.h"

#include "Engine/World.h"
#include "Navigation/NavGeometry.h"
#include "Navigation/NavigationSystem.h"
#include "Serialization/StructuredArchiveSlots.h"

#include <DetourAlloc.h>
#include <DetourCommon.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshQuery.h>

#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>

namespace
{
	constexpr int32 CurrentNavMeshDataVersion = 1;
	constexpr size_t MaxTileBytes = (std::numeric_limits<int32>::max)() / 2;

	FString EncodeTile(const TArray<uint8>& Bytes)
	{
		constexpr char Hex[] = "0123456789abcdef";

		FString Result;
		Result.reserve(static_cast<size_t>(Bytes.Num()) * 2);
		for (uint8 Byte : Bytes)
		{
			Result.push_back(Hex[Byte >> 4]);
			Result.push_back(Hex[Byte & 0x0f]);
		}
		return Result;
	}

	int DecodeHexDigit(char Digit)
	{
		if (Digit >= '0' && Digit <= '9')
			return Digit - '0';
		if (Digit >= 'a' && Digit <= 'f')
			return Digit - 'a' + 10;
		if (Digit >= 'A' && Digit <= 'F')
			return Digit - 'A' + 10;
		return -1;
	}

	bool DecodeTile(const FString& Encoded, TArray<uint8>& OutBytes)
	{
		const size_t ByteCount = Encoded.size() / 2;
		if (Encoded.size() % 2 != 0 ||
			ByteCount < sizeof(dtMeshHeader) || ByteCount > MaxTileBytes)
		{
			return false;
		}

		TArray<uint8> Bytes;
		Bytes.SetNum(static_cast<int32>(ByteCount));

		for (size_t Index = 0; Index < ByteCount; ++Index)
		{
			const int High = DecodeHexDigit(Encoded[Index * 2]);
			const int Low = DecodeHexDigit(Encoded[Index * 2 + 1]);
			if (High < 0 || Low < 0)
				return false;

			Bytes[static_cast<int32>(Index)] = static_cast<uint8>((High << 4) | Low);
		}

		OutBytes = std::move(Bytes);
		return true;
	}

	template <class T>
	T ReadTileValue(const uint8* Data, int64 Index = 0)
	{
		T Value{};
		std::memcpy(&Value, Data + Index * sizeof(T), sizeof(T));
		return Value;
	}

	bool ValidateTile(const TArray<uint8>& Bytes)
	{
		if (Bytes.Num() < static_cast<int32>(sizeof(dtMeshHeader)) ||
			static_cast<size_t>(Bytes.Num()) > MaxTileBytes)
		{
			return false;
		}

		const auto Header = ReadTileValue<dtMeshHeader>(Bytes.GetData());
		if (Header.magic != DT_NAVMESH_MAGIC ||
			Header.version != DT_NAVMESH_VERSION ||
			Header.x != 0 || Header.y != 0 || Header.layer != 0 ||
			Header.polyCount <= 0 || Header.polyCount > 0x7fff ||
			Header.vertCount < 3 || Header.vertCount >= 0xffff ||
			Header.maxLinkCount <= 0 ||
			Header.detailMeshCount != Header.polyCount ||
			Header.detailVertCount < 0 || Header.detailTriCount <= 0 ||
			Header.offMeshConCount != 0 || Header.offMeshBase != Header.polyCount ||
			(Header.bvNodeCount != 0 && Header.bvNodeCount != Header.polyCount * 2))
		{
			return false;
		}

		for (int Axis = 0; Axis < 3; ++Axis)
		{
			const float Extent = Header.bmax[Axis] - Header.bmin[Axis];
			if (!std::isfinite(Extent) || Extent < 0.0f ||
				(Axis != 1 && Extent == 0.0f))
			{
				return false;
			}
		}

		if (!std::isfinite(Header.bvQuantFactor) || Header.bvQuantFactor <= 0.0f ||
			!std::isfinite(Header.walkableHeight) || Header.walkableHeight <= 0.0f ||
			!std::isfinite(Header.walkableRadius) || Header.walkableRadius < 0.0f ||
			!std::isfinite(Header.walkableClimb) || Header.walkableClimb < 0.0f)
		{
			return false;
		}

		// Detour addTile()이 읽는 순서와 4바이트 정렬을 그대로 따른다.
		int64 Offset = 0;
		const auto TakeSection = [&](int64 Count, int64 Stride) -> const uint8*
			{
				const int64 AlignedSize = (Count * Stride + 3) & ~int64{ 3 };
				if (AlignedSize > Bytes.Num() - Offset)
					return nullptr;

				const uint8* Section = Bytes.GetData() + Offset;
				Offset += AlignedSize;
				return Section;
			};

		if (!TakeSection(1, sizeof(dtMeshHeader)))
			return false;

		const uint8* Vertices = TakeSection(Header.vertCount, 3 * sizeof(float));
		const uint8* Polys = TakeSection(Header.polyCount, sizeof(dtPoly));
		const uint8* Links = TakeSection(Header.maxLinkCount, sizeof(dtLink));
		const uint8* DetailMeshes = TakeSection(Header.detailMeshCount, sizeof(dtPolyDetail));
		const uint8* DetailVertices = TakeSection(Header.detailVertCount, 3 * sizeof(float));
		const uint8* DetailTriangles = TakeSection(Header.detailTriCount, 4 * sizeof(uint8));
		const uint8* BVNodes = TakeSection(Header.bvNodeCount, sizeof(dtBVNode));

		if (!Vertices || !Polys || !Links || !DetailMeshes ||
			!DetailVertices || !DetailTriangles || !BVNodes || Offset != Bytes.Num())
		{
			return false;
		}

		const auto AllFinite = [](const uint8* Data, int VertexCount)
			{
				for (int64 Index = 0; Index < 3LL * VertexCount; ++Index)
					if (!std::isfinite(ReadTileValue<float>(Data, Index)))
						return false;

				return true;
			};

		if (!AllFinite(Vertices, Header.vertCount) ||
			!AllFinite(DetailVertices, Header.detailVertCount))
		{
			return false;
		}

		for (int Index = 0; Index < Header.polyCount; ++Index)
		{
			const auto Poly = ReadTileValue<dtPoly>(Polys, Index);
			if (Poly.getType() != DT_POLYTYPE_GROUND ||
				Poly.vertCount < 3 || Poly.vertCount > DT_VERTS_PER_POLYGON)
			{
				return false;
			}

			for (int Edge = 0; Edge < Poly.vertCount; ++Edge)
			{
				const unsigned short Neighbor = Poly.neis[Edge];
				const bool bValidNeighbor = (Neighbor & DT_EXT_LINK)
					? (Neighbor & 0x7fff) <= 7
					: Neighbor <= Header.polyCount;

				if (Poly.verts[Edge] >= Header.vertCount || !bValidNeighbor)
					return false;
			}

			const auto Detail = ReadTileValue<dtPolyDetail>(DetailMeshes, Index);
			if (static_cast<int64>(Detail.vertBase) + Detail.vertCount > Header.detailVertCount ||
				static_cast<int64>(Detail.triBase) + Detail.triCount > Header.detailTriCount ||
				Detail.triCount == 0)
			{
				return false;
			}

			const int LocalVertexCount = Poly.vertCount + Detail.vertCount;
			for (int TriIndex = 0; TriIndex < Detail.triCount; ++TriIndex)
			{
				const uint8* Tri = DetailTriangles + 4LL * (Detail.triBase + TriIndex);
				if (Tri[0] >= LocalVertexCount ||
					Tri[1] >= LocalVertexCount ||
					Tri[2] >= LocalVertexCount)
				{
					return false;
				}
			}
		}

		for (int Index = 0; Index < Header.bvNodeCount; ++Index)
		{
			const auto Node = ReadTileValue<dtBVNode>(BVNodes, Index);
			if (Node.i >= Header.polyCount ||
				(Node.i < 0 && -static_cast<int64>(Node.i) > Header.bvNodeCount - Index))
			{
				return false;
			}
		}

		return true;
	}

	void BuildDebugGeometry(const dtMeshTile& Tile, FNavMeshDebugGeometry& Out)
	{
		const auto ToWorld = [](const float* P)
			{
				return Navigation::FromRecast(FVector(P[0], P[1], P[2]));
			};

		for (int PolyIndex = 0; PolyIndex < Tile.header->detailMeshCount; ++PolyIndex)
		{
			const dtPoly& Poly = Tile.polys[PolyIndex];
			const dtPolyDetail& Detail = Tile.detailMeshes[PolyIndex];

			for (int TriIndex = 0; TriIndex < Detail.triCount; ++TriIndex)
			{
				const unsigned char* Tri = Tile.detailTris + 4 * (Detail.triBase + TriIndex);
				const float* Positions[3];

				for (int Corner = 0; Corner < 3; ++Corner)
				{
					const int Index = Tri[Corner];
					Positions[Corner] = Index < Poly.vertCount
						? Tile.verts + 3 * Poly.verts[Index]
						: Tile.detailVerts + 3 * (Detail.vertBase + Index - Poly.vertCount);

					Out.FaceVertices.Add(ToWorld(Positions[Corner]));
				}

				for (int Edge = 0; Edge < 3; ++Edge)
				{
					if (!(dtGetDetailTriEdgeFlags(Tri[3], Edge) & DT_DETAIL_EDGE_BOUNDARY))
						continue;

					const float* A = Positions[Edge];
					const float* B = Positions[(Edge + 1) % 3];

					for (int Side = 0; Side < Poly.vertCount; ++Side)
					{
						if (Poly.neis[Side] != 0)
							continue;

						const float* Start = Tile.verts + 3 * Poly.verts[Side];
						const float* End = Tile.verts + 3 * Poly.verts[(Side + 1) % Poly.vertCount];

						constexpr float EdgeToleranceSquared = 1e-6f;
						float T = 0.0f;
						if (dtDistancePtSegSqr2D(A, Start, End, T) <= EdgeToleranceSquared &&
							dtDistancePtSegSqr2D(B, Start, End, T) <= EdgeToleranceSquared)
						{
							Out.BoundaryVertices.Add(ToWorld(A));
							Out.BoundaryVertices.Add(ToWorld(B));
							break;
						}
					}
				}
			}
		}
	}
}

struct FNavMeshData
{
	TArray<uint8> NavMeshTileData;
	FNavMeshDebugGeometry Debug;

	std::unique_ptr<dtNavMesh, decltype(&dtFreeNavMesh)>
		Mesh{ nullptr, &dtFreeNavMesh };

	// 멤버는 역순으로 파괴되므로 Query가 Mesh보다 먼저 해제된다.
	std::unique_ptr<dtNavMeshQuery, decltype(&dtFreeNavMeshQuery)>
		Query{ nullptr, &dtFreeNavMeshQuery };
};

ARecastNavMesh::ARecastNavMesh() = default;
ARecastNavMesh::~ARecastNavMesh() = default;

void ARecastNavMesh::Serialize(FStructuredArchive::FRecord Record)
{
	Super::Serialize(Record);

	FArchive& Ar = Record.GetUnderlyingArchive();
	const bool bWriteTile = Ar.IsSaving() && RecastNavMeshImpl &&
		!RecastNavMeshImpl->NavMeshTileData.IsEmpty();

	int32 Version = bWriteTile ? CurrentNavMeshDataVersion : 0;
	FString Encoded = bWriteTile ? EncodeTile(RecastNavMeshImpl->NavMeshTileData) : FString{};

	if (auto Field = Record.TryEnterField("NavMeshDataVersion", bWriteTile))
		Field.GetValue() << Version;
	if (auto Field = Record.TryEnterField("NavMeshTileData", bWriteTile))
		Field.GetValue() << Encoded;

	if (!Ar.IsLoading())
		return;

	RecastNavMeshImpl.reset();
	if (Version == 0 && Encoded.empty())
		return;

	TArray<uint8> TileBytes;
	if (Version != CurrentNavMeshDataVersion || !DecodeTile(Encoded, TileBytes))
	{
		KRS_LOG(Warning, "Navigation: ignored invalid or unsupported bake on {}", GetName());
		return;
	}

	// 씬 로드 또는 월드 복제가 끝난 뒤 Detour를 초기화한다.
	RecastNavMeshImpl = MakeUnique<FNavMeshData>();
	RecastNavMeshImpl->NavMeshTileData = std::move(TileBytes);
}

TUniquePtr<FNavMeshData> ARecastNavMesh::CreateNavMeshData(TArray<uint8> TileBytes)
{
	if (!ValidateTile(TileBytes))
	{
		KRS_LOG(Warning, "Navigation: rejected invalid Detour tile");
		return nullptr;
	}

	auto Data = MakeUnique<FNavMeshData>();
	Data->Mesh.reset(dtAllocNavMesh());
	Data->Query.reset(dtAllocNavMeshQuery());
	if (!Data->Mesh || !Data->Query)
		return nullptr;

	// Detour가 런타임 링크를 쓰는 버퍼. 저장용 원본과 독립적이다.
	std::unique_ptr<uint8, decltype(&dtFree)> TileMemory(
		static_cast<uint8*>(dtAlloc(TileBytes.Num(), DT_ALLOC_PERM)), &dtFree);
	if (!TileMemory)
		return nullptr;

	std::memcpy(TileMemory.get(), TileBytes.GetData(), TileBytes.Num());
	if (dtStatusFailed(Data->Mesh->init(
		TileMemory.get(), TileBytes.Num(), DT_TILE_FREE_DATA)))
	{
		return nullptr;
	}

	TileMemory.release();

	if (dtStatusFailed(Data->Query->init(Data->Mesh.get(), 2048)))
		return nullptr;

	const dtMeshTile* Tile = Data->Mesh->getTileAt(0, 0, 0);
	if (!Tile || !Tile->header)
		return nullptr;

	BuildDebugGeometry(*Tile, Data->Debug);
	if (Data->Debug.FaceVertices.IsEmpty())
		return nullptr;

	Data->NavMeshTileData = std::move(TileBytes);
	return Data;
}

ARecastNavMesh* ARecastNavMesh::InstallBakedTile(
	UWorld& World, ARecastNavMesh* ExistingNavMesh, TArray<uint8> TileBytes)
{
	auto Data = CreateNavMeshData(std::move(TileBytes));
	if (!Data)
		return nullptr;

	// 타일 준비가 성공한 뒤 액터를 만들거나 갱신한다.
	ARecastNavMesh* NavMesh = ExistingNavMesh;
	if (!NavMesh)
		NavMesh = World.SpawnActor<ARecastNavMesh>(FName("RecastNavMesh-Default"));

	if (NavMesh)
		NavMesh->RecastNavMeshImpl = std::move(Data);

	return NavMesh;
}

bool ARecastNavMesh::InitializeFromStoredTile()
{
	if (!RecastNavMeshImpl || RecastNavMeshImpl->NavMeshTileData.IsEmpty())
		return false;

	if (RecastNavMeshImpl->Query)
		return true;

	auto Data = CreateNavMeshData(std::move(RecastNavMeshImpl->NavMeshTileData));
	RecastNavMeshImpl = std::move(Data);
	return RecastNavMeshImpl != nullptr;
}

void ARecastNavMesh::ClearNavMeshData()
{
	RecastNavMeshImpl.reset();
}

const FNavMeshDebugGeometry* ARecastNavMesh::GetDebugGeometry() const
{
	return GetNavMeshQuery() ? &RecastNavMeshImpl->Debug : nullptr;
}

const dtNavMeshQuery* ARecastNavMesh::GetNavMeshQuery() const
{
	return RecastNavMeshImpl ? RecastNavMeshImpl->Query.get() : nullptr;
}
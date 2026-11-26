#include "EnginePCH.h"
#include "Render/StockingRegion.h"
#include "Core/Misc/Paths.h"
#include "Serialization/StructuredArchive.h"
#include "json.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace
{
	uint64 MixHashValue(uint64 Hash, uint64 Value)
	{
		Hash ^= Value;
		Hash *= 1099511628211ull;
		return Hash;
	}

	uint64 MakeEdgeKey(uint32 VertexA, uint32 VertexB)
	{
		const uint32 Lo = std::min(VertexA, VertexB);
		const uint32 Hi = std::max(VertexA, VertexB);
		return (static_cast<uint64>(Lo) << 32) | static_cast<uint64>(Hi);
	}

	float ComputeVerticesReferenceScale(const TArray<FVertex>& Vertices)
	{
		if (Vertices.IsEmpty())
		{
			return 1.0f;
		}
		FVector MinPt(FLT_MAX, FLT_MAX, FLT_MAX);
		FVector MaxPt(-FLT_MAX, -FLT_MAX, -FLT_MAX);
		for (int32 v = 0; v < Vertices.Num(); ++v)
		{
			const FVector& P = Vertices[v].Position;
			MinPt.X = std::min(MinPt.X, P.X);
			MinPt.Y = std::min(MinPt.Y, P.Y);
			MinPt.Z = std::min(MinPt.Z, P.Z);
			MaxPt.X = std::max(MaxPt.X, P.X);
			MaxPt.Y = std::max(MaxPt.Y, P.Y);
			MaxPt.Z = std::max(MaxPt.Z, P.Z);
		}
		const FVector Extent = MaxPt - MinPt;
		const float Height = Extent.Z > 1e-4f
			? Extent.Z
			: std::max(Extent.X, std::max(Extent.Y, Extent.Z));
		return std::max(Height / 1.75f, 0.05f);
	}

	uint64 MakeQuantizedPositionKey(const FVector& Pos, float QuantScale)
	{
		// 동일 좌표로 분리된 심 정점을 하나로 취급하기 위한 양자화 키
		const int64 QX = static_cast<int64>(std::llround(Pos.X * QuantScale));
		const int64 QY = static_cast<int64>(std::llround(Pos.Y * QuantScale));
		const int64 QZ = static_cast<int64>(std::llround(Pos.Z * QuantScale));
		uint64 Hash = 14695981039346656037ull;
		Hash = MixHashValue(Hash, static_cast<uint64>(QX));
		Hash = MixHashValue(Hash, static_cast<uint64>(QY));
		Hash = MixHashValue(Hash, static_cast<uint64>(QZ));
		return Hash;
	}

	void BuildWeldedVertexIds(const TArray<FVertex>& Vertices, TArray<uint32>& OutWeldedIds)
	{
		const int32 VertexCount = Vertices.Num();
		OutWeldedIds.SetNum(VertexCount);
		const float RefScale = ComputeVerticesReferenceScale(Vertices);
		const float QuantScale = 10000.0f / RefScale;
		std::unordered_map<uint64, uint32> PosToId;
		PosToId.reserve(static_cast<size_t>(VertexCount));
		uint32 NextId = 0;

		for (int32 v = 0; v < VertexCount; ++v)
		{
			const uint64 Key = MakeQuantizedPositionKey(Vertices[v].Position, QuantScale);
			const std::unordered_map<uint64, uint32>::const_iterator Found = PosToId.find(Key);
			if (Found != PosToId.end())
			{
				OutWeldedIds[v] = Found->second;
			}
			else
			{
				PosToId.emplace(Key, NextId);
				OutWeldedIds[v] = NextId;
				++NextId;
			}
		}
	}

	FSkinWeight BlendSkinWeights(const FSkinWeight& WeightA, const FSkinWeight& WeightB)
	{
		struct FInfluencePair
		{
			uint8 BoneIndex = 0;
			float Weight = 0.0f;
		};

		TArray<FInfluencePair> Combined;
		Combined.Reserve(MaxBoneInfluences * 2);

		const std::function<void(uint8, float)> Accumulate = [&](uint8 Bone, float Weight)
		{
			if (Weight <= 0.0f)
			{
				return;
			}
			for (int32 i = 0; i < Combined.Num(); ++i)
			{
				if (Combined[i].BoneIndex == Bone)
				{
					Combined[i].Weight += Weight;
					return;
				}
			}
			Combined.Add({ Bone, Weight });
		};

		for (int32 k = 0; k < MaxBoneInfluences; ++k)
		{
			Accumulate(WeightA.BoneIndex[k], WeightA.Weight[k] * 0.5f);
			Accumulate(WeightB.BoneIndex[k], WeightB.Weight[k] * 0.5f);
		}

		std::sort(Combined.begin(), Combined.end(), [](const FInfluencePair& L, const FInfluencePair& R)
		{
			return L.Weight > R.Weight;
		});

		FSkinWeight Result{};
		float TotalWeight = 0.0f;
		const int32 Count = std::min(Combined.Num(), MaxBoneInfluences);
		for (int32 k = 0; k < Count; ++k)
		{
			Result.BoneIndex[k] = Combined[k].BoneIndex;
			Result.Weight[k] = Combined[k].Weight;
			TotalWeight += Combined[k].Weight;
		}
		if (TotalWeight > 1e-6f)
		{
			for (int32 k = 0; k < Count; ++k)
			{
				Result.Weight[k] /= TotalWeight;
			}
		}
		else
		{
			Result.BoneIndex[0] = WeightA.BoneIndex[0];
			Result.Weight[0] = 1.0f;
		}
		return Result;
	}

	FVertex InterpolateMidpointVertex(const FVertex& VertexA, const FVertex& VertexB)
	{
		FVertex Mid{};
		Mid.Position = (VertexA.Position + VertexB.Position) * 0.5f;
		const FVector SumNormal = VertexA.Normal + VertexB.Normal;
		Mid.Normal = SumNormal.SizeSquared() > 1e-10f ? SumNormal.Normalized() : VertexA.Normal;
		const FVector SumTangent(
			VertexA.Tangent.X + VertexB.Tangent.X,
			VertexA.Tangent.Y + VertexB.Tangent.Y,
			VertexA.Tangent.Z + VertexB.Tangent.Z);
		const FVector NormTangent = SumTangent.SizeSquared() > 1e-10f
			? SumTangent.Normalized()
			: FVector(VertexA.Tangent.X, VertexA.Tangent.Y, VertexA.Tangent.Z);
		Mid.Tangent = FVector4(NormTangent.X, NormTangent.Y, NormTangent.Z, VertexA.Tangent.W);
		Mid.UV = (VertexA.UV + VertexB.UV) * 0.5f;
		Mid.Color = (VertexA.Color + VertexB.Color) * 0.5f;
		return Mid;
	}

	void SubdivideTrianglesOnce(TArray<FVertex>& InOutVertices, TArray<FSkinWeight>& InOutWeights, TArray<uint32>& InOutIndices)
	{
		std::unordered_map<uint64, uint32> EdgeMidpoints;
		TArray<uint32> NewIndices;
		NewIndices.Reserve(InOutIndices.Num() * 4);

		const std::function<uint32(uint32, uint32)> GetOrCreateMidpoint = [&](uint32 IndexA, uint32 IndexB) -> uint32
		{
			const uint64 Key = MakeEdgeKey(IndexA, IndexB);
			const std::unordered_map<uint64, uint32>::const_iterator Found = EdgeMidpoints.find(Key);
			if (Found != EdgeMidpoints.end())
			{
				return Found->second;
			}
			const uint32 NewIndex = static_cast<uint32>(InOutVertices.Num());
			InOutVertices.Add(InterpolateMidpointVertex(InOutVertices[IndexA], InOutVertices[IndexB]));
			InOutWeights.Add(BlendSkinWeights(InOutWeights[IndexA], InOutWeights[IndexB]));
			EdgeMidpoints.emplace(Key, NewIndex);
			return NewIndex;
		};

		for (int32 i = 0; i + 2 < InOutIndices.Num(); i += 3)
		{
			const uint32 I0 = InOutIndices[i];
			const uint32 I1 = InOutIndices[i + 1];
			const uint32 I2 = InOutIndices[i + 2];

			const uint32 M01 = GetOrCreateMidpoint(I0, I1);
			const uint32 M12 = GetOrCreateMidpoint(I1, I2);
			const uint32 M20 = GetOrCreateMidpoint(I2, I0);

			NewIndices.Add(I0); NewIndices.Add(M01); NewIndices.Add(M20);
			NewIndices.Add(M01); NewIndices.Add(I1); NewIndices.Add(M12);
			NewIndices.Add(M20); NewIndices.Add(M12); NewIndices.Add(I2);
			NewIndices.Add(M01); NewIndices.Add(M12); NewIndices.Add(M20);
		}

		InOutIndices = std::move(NewIndices);
	}

	// 긴 삼각형 변만 분할해 저폴리곤 구간에서도 매끄러운 단일 핀치 정점 밀도 확보
	void SubdivideLongEdgesAdaptive(
		TArray<FVertex>& InOutVertices,
		TArray<FSkinWeight>& InOutWeights,
		TArray<uint32>& InOutIndices,
		float MaxEdgeLength,
		int32 MaxPasses,
		int32 MaxTriangles)
	{
		if (MaxEdgeLength <= 1e-5f || MaxPasses <= 0 || InOutIndices.Num() < 3)
		{
			return;
		}

		const float MaxEdgeSq = MaxEdgeLength * MaxEdgeLength;
		for (int32 Pass = 0; Pass < MaxPasses; ++Pass)
		{
			const int32 CurrentTriCount = InOutIndices.Num() / 3;
			if (CurrentTriCount >= MaxTriangles)
			{
				break;
			}

			std::unordered_map<uint64, uint32> EdgeMidpoints;
			TArray<uint32> NewIndices;
			NewIndices.Reserve(InOutIndices.Num() * 2);
			bool bSplitAny = false;

			const std::function<uint32(uint32, uint32)> GetOrCreateMidpoint = [&](uint32 IndexA, uint32 IndexB) -> uint32
			{
				const uint64 Key = MakeEdgeKey(IndexA, IndexB);
				const std::unordered_map<uint64, uint32>::const_iterator Found = EdgeMidpoints.find(Key);
				if (Found != EdgeMidpoints.end())
				{
					return Found->second;
				}
				const uint32 NewIndex = static_cast<uint32>(InOutVertices.Num());
				InOutVertices.Add(InterpolateMidpointVertex(InOutVertices[IndexA], InOutVertices[IndexB]));
				InOutWeights.Add(BlendSkinWeights(InOutWeights[IndexA], InOutWeights[IndexB]));
				EdgeMidpoints.emplace(Key, NewIndex);
				return NewIndex;
			};

			const std::function<void(uint32, uint32, uint32, uint32)> EmitSplitQuad =
				[&](uint32 A, uint32 B, uint32 C, uint32 D)
			{
				const float DiagAC = (InOutVertices[A].Position - InOutVertices[C].Position).SizeSquared();
				const float DiagBD = (InOutVertices[B].Position - InOutVertices[D].Position).SizeSquared();
				if (DiagAC <= DiagBD)
				{
					NewIndices.Add(A); NewIndices.Add(B); NewIndices.Add(C);
					NewIndices.Add(A); NewIndices.Add(C); NewIndices.Add(D);
				}
				else
				{
					NewIndices.Add(A); NewIndices.Add(B); NewIndices.Add(D);
					NewIndices.Add(B); NewIndices.Add(C); NewIndices.Add(D);
				}
			};

			for (int32 i = 0; i + 2 < InOutIndices.Num(); i += 3)
			{
				const uint32 I0 = InOutIndices[i];
				const uint32 I1 = InOutIndices[i + 1];
				const uint32 I2 = InOutIndices[i + 2];

				const bool bSplit01 = (InOutVertices[I0].Position - InOutVertices[I1].Position).SizeSquared() > MaxEdgeSq;
				const bool bSplit12 = (InOutVertices[I1].Position - InOutVertices[I2].Position).SizeSquared() > MaxEdgeSq;
				const bool bSplit20 = (InOutVertices[I2].Position - InOutVertices[I0].Position).SizeSquared() > MaxEdgeSq;
				const int32 SplitCount = (bSplit01 ? 1 : 0) + (bSplit12 ? 1 : 0) + (bSplit20 ? 1 : 0);

				if (SplitCount == 0)
				{
					NewIndices.Add(I0); NewIndices.Add(I1); NewIndices.Add(I2);
					continue;
				}

				bSplitAny = true;
				if (SplitCount == 3)
				{
					const uint32 M01 = GetOrCreateMidpoint(I0, I1);
					const uint32 M12 = GetOrCreateMidpoint(I1, I2);
					const uint32 M20 = GetOrCreateMidpoint(I2, I0);
					NewIndices.Add(I0); NewIndices.Add(M01); NewIndices.Add(M20);
					NewIndices.Add(M01); NewIndices.Add(I1); NewIndices.Add(M12);
					NewIndices.Add(M20); NewIndices.Add(M12); NewIndices.Add(I2);
					NewIndices.Add(M01); NewIndices.Add(M12); NewIndices.Add(M20);
				}
				else if (SplitCount == 1)
				{
					if (bSplit01)
					{
						const uint32 M01 = GetOrCreateMidpoint(I0, I1);
						NewIndices.Add(I0); NewIndices.Add(M01); NewIndices.Add(I2);
						NewIndices.Add(M01); NewIndices.Add(I1); NewIndices.Add(I2);
					}
					else if (bSplit12)
					{
						const uint32 M12 = GetOrCreateMidpoint(I1, I2);
						NewIndices.Add(I0); NewIndices.Add(I1); NewIndices.Add(M12);
						NewIndices.Add(I0); NewIndices.Add(M12); NewIndices.Add(I2);
					}
					else
					{
						const uint32 M20 = GetOrCreateMidpoint(I2, I0);
						NewIndices.Add(I0); NewIndices.Add(I1); NewIndices.Add(M20);
						NewIndices.Add(M20); NewIndices.Add(I1); NewIndices.Add(I2);
					}
				}
				else
				{
					if (!bSplit01)
					{
						const uint32 M12 = GetOrCreateMidpoint(I1, I2);
						const uint32 M20 = GetOrCreateMidpoint(I2, I0);
						NewIndices.Add(M20); NewIndices.Add(M12); NewIndices.Add(I2);
						EmitSplitQuad(I0, I1, M12, M20);
					}
					else if (!bSplit12)
					{
						const uint32 M01 = GetOrCreateMidpoint(I0, I1);
						const uint32 M20 = GetOrCreateMidpoint(I2, I0);
						NewIndices.Add(I0); NewIndices.Add(M01); NewIndices.Add(M20);
						EmitSplitQuad(M01, I1, I2, M20);
					}
					else
					{
						const uint32 M01 = GetOrCreateMidpoint(I0, I1);
						const uint32 M12 = GetOrCreateMidpoint(I1, I2);
						NewIndices.Add(M01); NewIndices.Add(I1); NewIndices.Add(M12);
						EmitSplitQuad(I0, M01, M12, I2);
					}
				}
			}

			InOutIndices = std::move(NewIndices);
			if (!bSplitAny)
			{
				break;
			}
		}
	}

	// 용접된 위치 기준으로 실제 열린 경계만 검출해 끝단 마스크와 높이 정규화 계산
	void WriteStockingVertexMasks(TArray<FVertex>& Vertices, const TArray<uint32>& Indices, float FeatherDistance)
	{
		const int32 VertexCount = Vertices.Num();
		if (VertexCount == 0 || Indices.Num() < 3)
		{
			return;
		}

		TArray<uint32> WeldedIds;
		BuildWeldedVertexIds(Vertices, WeldedIds);

		std::unordered_map<uint64, int32> EdgeUseCount;
		EdgeUseCount.reserve(static_cast<size_t>(Indices.Num()));
		for (int32 i = 0; i + 2 < Indices.Num(); i += 3)
		{
			const uint32 Tri[3] = {
				WeldedIds[Indices[i]],
				WeldedIds[Indices[i + 1]],
				WeldedIds[Indices[i + 2]]
			};
			for (int32 e = 0; e < 3; ++e)
			{
				if (Tri[e] == Tri[(e + 1) % 3])
				{
					continue;
				}
				const uint64 Key = MakeEdgeKey(Tri[e], Tri[(e + 1) % 3]);
				EdgeUseCount[Key] += 1;
			}
		}

		TArray<FVector> BoundaryPositions;
		float MinZ = FLT_MAX;
		float MaxZ = -FLT_MAX;
		for (int32 v = 0; v < VertexCount; ++v)
		{
			MinZ = FMath::Min(MinZ, Vertices[v].Position.Z);
			MaxZ = FMath::Max(MaxZ, Vertices[v].Position.Z);
		}

		std::unordered_set<uint32> BoundaryWeldedIds;
		for (const std::pair<const uint64, int32>& Pair : EdgeUseCount)
		{
			if (Pair.second == 1)
			{
				const uint32 Lo = static_cast<uint32>(Pair.first >> 32);
				const uint32 Hi = static_cast<uint32>(Pair.first & 0xFFFFFFFFull);
				BoundaryWeldedIds.insert(Lo);
				BoundaryWeldedIds.insert(Hi);
			}
		}

		std::unordered_set<uint32> RecordedBoundaryIds;
		for (int32 v = 0; v < VertexCount; ++v)
		{
			const uint32 Wid = WeldedIds[v];
			if (BoundaryWeldedIds.contains(Wid) && RecordedBoundaryIds.insert(Wid).second)
			{
				BoundaryPositions.Add(Vertices[v].Position);
			}
		}

		const float SafeFeather = std::max(FeatherDistance, 1e-4f);
		const float HeightSpan = std::max(MaxZ - MinZ, 1e-4f);

		for (int32 v = 0; v < VertexCount; ++v)
		{
			float EdgeMask = 1.0f;
			if (!BoundaryPositions.IsEmpty())
			{
				float MinDistSq = FLT_MAX;
				const FVector& Pos = Vertices[v].Position;
				for (int32 b = 0; b < BoundaryPositions.Num(); ++b)
				{
					const float DistSq = (Pos - BoundaryPositions[b]).SizeSquared();
					if (DistSq < MinDistSq)
					{
						MinDistSq = DistSq;
					}
				}
				const float Dist = std::sqrt(MinDistSq);
				EdgeMask = std::clamp(Dist / SafeFeather, 0.001f, 1.0f);
			}

			const float NormalizedHeight = std::clamp((Vertices[v].Position.Z - MinZ) / HeightSpan, 0.0f, 1.0f);
			// 빨강은 경계 마스크 초록은 높이 비율 파랑은 중립 주름 알파는 기본 신장 비율
			Vertices[v].Color = FVector4(EdgeMask, NormalizedHeight, 0.5f, 1.0f);
		}
	}

	// 연결 조각별 비겹침 원통형 아틀라스 좌표를 생성해 모든 모델에서 세로 올 풀림 방향 보장
	void GenerateStockingAtlasUVs(
		TArray<FVertex>& InOutVertices,
		TArray<FSkinWeight>& InOutWeights,
		TArray<uint32>& InOutIndices)
	{
		const int32 OrigVertCount = InOutVertices.Num();
		const int32 TriCount = InOutIndices.Num() / 3;
		if (OrigVertCount == 0 || TriCount == 0)
		{
			return;
		}

		TArray<uint32> WeldedIds;
		BuildWeldedVertexIds(InOutVertices, WeldedIds);
		uint32 WeldedCount = 0;
		for (int32 v = 0; v < OrigVertCount; ++v)
		{
			WeldedCount = std::max(WeldedCount, WeldedIds[v] + 1u);
		}

		TArray<TArray<uint32>> Adj;
		Adj.SetNum(static_cast<int32>(WeldedCount));
		for (int32 t = 0; t < TriCount; ++t)
		{
			const uint32 W0 = WeldedIds[InOutIndices[t * 3]];
			const uint32 W1 = WeldedIds[InOutIndices[t * 3 + 1]];
			const uint32 W2 = WeldedIds[InOutIndices[t * 3 + 2]];
			if (W0 != W1) { Adj[static_cast<int32>(W0)].Add(W1); Adj[static_cast<int32>(W1)].Add(W0); }
			if (W1 != W2) { Adj[static_cast<int32>(W1)].Add(W2); Adj[static_cast<int32>(W2)].Add(W1); }
			if (W2 != W0) { Adj[static_cast<int32>(W2)].Add(W0); Adj[static_cast<int32>(W0)].Add(W2); }
		}

		TArray<int32> WeldedComp;
		WeldedComp.SetNum(static_cast<int32>(WeldedCount));
		for (uint32 w = 0; w < WeldedCount; ++w)
		{
			WeldedComp[static_cast<int32>(w)] = -1;
		}

		int32 CompCount = 0;
		TArray<uint32> Queue;
		for (uint32 w = 0; w < WeldedCount; ++w)
		{
			if (WeldedComp[static_cast<int32>(w)] >= 0)
			{
				continue;
			}
			const int32 Cid = CompCount++;
			WeldedComp[static_cast<int32>(w)] = Cid;
			Queue.Reset();
			Queue.Add(w);
			for (int32 Head = 0; Head < Queue.Num(); ++Head)
			{
				const uint32 Cur = Queue[Head];
				const TArray<uint32>& Neighbors = Adj[static_cast<int32>(Cur)];
				for (int32 n = 0; n < Neighbors.Num(); ++n)
				{
					const uint32 Next = Neighbors[n];
					if (WeldedComp[static_cast<int32>(Next)] < 0)
					{
						WeldedComp[static_cast<int32>(Next)] = Cid;
						Queue.Add(Next);
					}
				}
			}
		}

		if (CompCount <= 0)
		{
			return;
		}

		// 골반으로 연결된 좌우 다리 한 덩어리를 감지해 각 다리 내부 중심선 기준의 독립 타일로 분리
		TArray<int32> CompVertCount;
		TArray<float> CompMinX;
		TArray<float> CompMaxX;
		TArray<float> CompMinY;
		TArray<float> CompMaxY;
		TArray<float> CompMinZ;
		TArray<float> CompMaxZ;
		CompVertCount.SetNum(CompCount);
		CompMinX.SetNum(CompCount);
		CompMaxX.SetNum(CompCount);
		CompMinY.SetNum(CompCount);
		CompMaxY.SetNum(CompCount);
		CompMinZ.SetNum(CompCount);
		CompMaxZ.SetNum(CompCount);
		for (int32 c = 0; c < CompCount; ++c)
		{
			CompVertCount[c] = 0;
			CompMinX[c] = FLT_MAX;
			CompMaxX[c] = -FLT_MAX;
			CompMinY[c] = FLT_MAX;
			CompMaxY[c] = -FLT_MAX;
			CompMinZ[c] = FLT_MAX;
			CompMaxZ[c] = -FLT_MAX;
		}

		for (int32 v = 0; v < OrigVertCount; ++v)
		{
			const int32 Cid = WeldedComp[static_cast<int32>(WeldedIds[v])];
			if (Cid < 0 || Cid >= CompCount)
			{
				continue;
			}
			const FVector& Pos = InOutVertices[v].Position;
			CompVertCount[Cid] += 1;
			CompMinX[Cid] = std::min(CompMinX[Cid], Pos.X);
			CompMaxX[Cid] = std::max(CompMaxX[Cid], Pos.X);
			CompMinY[Cid] = std::min(CompMinY[Cid], Pos.Y);
			CompMaxY[Cid] = std::max(CompMaxY[Cid], Pos.Y);
			CompMinZ[Cid] = std::min(CompMinZ[Cid], Pos.Z);
			CompMaxZ[Cid] = std::max(CompMaxZ[Cid], Pos.Z);
		}

		TArray<int32> VertexTileId;
		VertexTileId.SetNum(OrigVertCount);
		int32 NextTileId = 0;
		TArray<int32> CompBaseTile;
		TArray<int32> CompRightTile;
		TArray<int32> CompSplitAxis;
		TArray<float> CompSplitCoord;
		CompBaseTile.SetNum(CompCount);
		CompRightTile.SetNum(CompCount);
		CompSplitAxis.SetNum(CompCount);
		CompSplitCoord.SetNum(CompCount);

		for (int32 c = 0; c < CompCount; ++c)
		{
			CompBaseTile[c] = -1;
			CompRightTile[c] = -1;
			CompSplitAxis[c] = -1;
			CompSplitCoord[c] = 0.0f;

			if (CompVertCount[c] < 16)
			{
				continue;
			}

			const float SpanZ = CompMaxZ[c] - CompMinZ[c];
			int32 BestAxis = -1;
			float BestSplitVal = 0.0f;
			float BestScore = -1.0f;

			// 하부에서 중간 높이 구간에 중앙 빈 틈이 있는 수평 축을 자동 탐색해 좌우 다리로 분할
			if (CompVertCount[c] >= 64 && SpanZ > 1e-4f)
			{
				const float LowZ = CompMinZ[c] + SpanZ * 0.12f;
				const float HighZ = CompMinZ[c] + SpanZ * 0.68f;
				const float AxisMin[2] = { CompMinX[c], CompMinY[c] };
				const float AxisMax[2] = { CompMaxX[c], CompMaxY[c] };

				for (int32 Axis = 0; Axis < 2; ++Axis)
				{
					const float Span = AxisMax[Axis] - AxisMin[Axis];
					if (Span <= 1e-4f)
					{
						continue;
					}
					const float Mid = 0.5f * (AxisMin[Axis] + AxisMax[Axis]);
					const float GapHalf = Span * 0.055f;
					int32 LowerCount = 0;
					int32 NegCount = 0;
					int32 PosCount = 0;
					int32 GapCount = 0;

					for (int32 v = 0; v < OrigVertCount; ++v)
					{
						if (WeldedComp[static_cast<int32>(WeldedIds[v])] != c)
						{
							continue;
						}
						const FVector& Pos = InOutVertices[v].Position;
						if (Pos.Z < LowZ || Pos.Z > HighZ)
						{
							continue;
						}
						const float Coord = (Axis == 0) ? Pos.X : Pos.Y;
						LowerCount += 1;
						if (std::fabs(Coord - Mid) <= GapHalf)
						{
							GapCount += 1;
						}
						else if (Coord < Mid)
						{
							NegCount += 1;
						}
						else
						{
							PosCount += 1;
						}
					}

					if (LowerCount >= 24 && NegCount >= 8 && PosCount >= 8 &&
						GapCount * 14 <= LowerCount)
					{
						const float EmptyRatio = 1.0f - static_cast<float>(GapCount) / static_cast<float>(LowerCount);
						const float Score = EmptyRatio * Span;
						if (Score > BestScore)
						{
							BestScore = Score;
							BestAxis = Axis;
							BestSplitVal = Mid;
						}
					}
				}
			}

			CompBaseTile[c] = NextTileId++;
			CompSplitAxis[c] = BestAxis;
			CompSplitCoord[c] = BestSplitVal;
			if (BestAxis >= 0)
			{
				CompRightTile[c] = NextTileId++;
			}
		}

		// 모든 조각이 작아 타일이 비었으면 첫 조각에 기본 타일 할당
		if (NextTileId == 0)
		{
			CompBaseTile[0] = NextTileId++;
		}

		for (int32 v = 0; v < OrigVertCount; ++v)
		{
			const int32 Cid = WeldedComp[static_cast<int32>(WeldedIds[v])];
			if (Cid >= 0 && Cid < CompCount && CompBaseTile[Cid] >= 0)
			{
				const int32 Axis = CompSplitAxis[Cid];
				const float Coord = (Axis == 0) ? InOutVertices[v].Position.X : InOutVertices[v].Position.Y;
				if (CompRightTile[Cid] >= 0 && Axis >= 0 && Coord >= CompSplitCoord[Cid])
				{
					VertexTileId[v] = CompRightTile[Cid];
				}
				else
				{
					VertexTileId[v] = CompBaseTile[Cid];
				}
			}
			else
			{
				VertexTileId[v] = 0;
			}
		}

		// 좌우 분할 경계를 가로지르는 골반 연결 삼각형은 다수 쪽 타일로 복제해 타일 간 UV 늘어짐 차단
		std::unordered_map<uint64, uint32> TileBoundaryDups;
		for (int32 t = 0; t < TriCount; ++t)
		{
			const uint32 I0 = InOutIndices[t * 3];
			const uint32 I1 = InOutIndices[t * 3 + 1];
			const uint32 I2 = InOutIndices[t * 3 + 2];
			const int32 T0 = VertexTileId[static_cast<int32>(I0)];
			const int32 T1 = VertexTileId[static_cast<int32>(I1)];
			const int32 T2 = VertexTileId[static_cast<int32>(I2)];
			if (T0 == T1 && T1 == T2)
			{
				continue;
			}

			int32 TargetTile = T0;
			if (T1 == T2)
			{
				TargetTile = T1;
			}
			else if (T0 == T2)
			{
				TargetTile = T0;
			}

			for (int32 c = 0; c < 3; ++c)
			{
				const uint32 OrigIdx = InOutIndices[t * 3 + c];
				if (VertexTileId[static_cast<int32>(OrigIdx)] == TargetTile)
				{
					continue;
				}
				const uint64 DupKey = (static_cast<uint64>(OrigIdx) << 32) | static_cast<uint64>(static_cast<uint32>(TargetTile));
				const std::unordered_map<uint64, uint32>::const_iterator Found = TileBoundaryDups.find(DupKey);
				if (Found != TileBoundaryDups.end())
				{
					InOutIndices[t * 3 + c] = Found->second;
				}
				else
				{
					const uint32 NewIdx = static_cast<uint32>(InOutVertices.Num());
					InOutVertices.Add(InOutVertices[static_cast<int32>(OrigIdx)]);
					InOutWeights.Add(InOutWeights[static_cast<int32>(OrigIdx)]);
					VertexTileId.Add(TargetTile);
					TileBoundaryDups.emplace(DupKey, NewIdx);
					InOutIndices[t * 3 + c] = NewIdx;
				}
			}
		}

		const int32 TileCount = NextTileId;
		const int32 PostSplitVertCount = InOutVertices.Num();

		constexpr int32 SliceCount = 24;
		struct FCompStats
		{
			int32 OrigId = 0;
			float MinZ = FLT_MAX;
			float MaxZ = -FLT_MAX;
			float SumX = 0.0f;
			float SumY = 0.0f;
			int32 Count = 0;
			float SliceSumX[SliceCount] = {};
			float SliceSumY[SliceCount] = {};
			int32 SliceCountArr[SliceCount] = {};
			float SliceCenterX[SliceCount] = {};
			float SliceCenterY[SliceCount] = {};
		};

		TArray<FCompStats> Stats;
		Stats.SetNum(TileCount);
		for (int32 c = 0; c < TileCount; ++c)
		{
			Stats[c].OrigId = c;
		}

		for (int32 v = 0; v < PostSplitVertCount; ++v)
		{
			const int32 Tid = VertexTileId[v];
			if (Tid < 0 || Tid >= TileCount)
			{
				continue;
			}
			const FVector& Pos = InOutVertices[v].Position;
			Stats[Tid].MinZ = std::min(Stats[Tid].MinZ, Pos.Z);
			Stats[Tid].MaxZ = std::max(Stats[Tid].MaxZ, Pos.Z);
			Stats[Tid].SumX += Pos.X;
			Stats[Tid].SumY += Pos.Y;
			Stats[Tid].Count += 1;
		}

		for (int32 v = 0; v < PostSplitVertCount; ++v)
		{
			const int32 Tid = VertexTileId[v];
			if (Tid < 0 || Tid >= TileCount)
			{
				continue;
			}
			FCompStats& S = Stats[Tid];
			const float SpanZ = std::max(S.MaxZ - S.MinZ, 1e-4f);
			const float NormZ = std::clamp((InOutVertices[v].Position.Z - S.MinZ) / SpanZ, 0.0f, 1.0f);
			const int32 Slice = std::clamp(static_cast<int32>(NormZ * static_cast<float>(SliceCount - 1)), 0, SliceCount - 1);
			S.SliceSumX[Slice] += InOutVertices[v].Position.X;
			S.SliceSumY[Slice] += InOutVertices[v].Position.Y;
			S.SliceCountArr[Slice] += 1;
		}

		for (int32 c = 0; c < TileCount; ++c)
		{
			FCompStats& S = Stats[c];
			const float FallbackX = S.Count > 0 ? (S.SumX / static_cast<float>(S.Count)) : 0.0f;
			const float FallbackY = S.Count > 0 ? (S.SumY / static_cast<float>(S.Count)) : 0.0f;
			for (int32 s = 0; s < SliceCount; ++s)
			{
				if (S.SliceCountArr[s] > 0)
				{
					S.SliceCenterX[s] = S.SliceSumX[s] / static_cast<float>(S.SliceCountArr[s]);
					S.SliceCenterY[s] = S.SliceSumY[s] / static_cast<float>(S.SliceCountArr[s]);
				}
				else
				{
					S.SliceCenterX[s] = FallbackX;
					S.SliceCenterY[s] = FallbackY;
				}
			}
			// 비어 있는 높이 슬라이스는 가장 가까운 유효 슬라이스 중심으로 보간
			for (int32 s = 0; s < SliceCount; ++s)
			{
				if (S.SliceCountArr[s] > 0)
				{
					continue;
				}
				int32 Nearest = -1;
				int32 BestDist = SliceCount + 1;
				for (int32 k = 0; k < SliceCount; ++k)
				{
					if (S.SliceCountArr[k] > 0)
					{
						const int32 Dist = std::abs(k - s);
						if (Dist < BestDist)
						{
							BestDist = Dist;
							Nearest = k;
						}
					}
				}
				if (Nearest >= 0)
				{
					S.SliceCenterX[s] = S.SliceCenterX[Nearest];
					S.SliceCenterY[s] = S.SliceCenterY[Nearest];
				}
			}
		}

		// 조각 중심 좌표 기준으로 타일 순서를 고정해 좌우 다리 배치를 일정하게 유지
		TArray<int32> SortedOrder;
		SortedOrder.SetNum(TileCount);
		for (int32 c = 0; c < TileCount; ++c)
		{
			SortedOrder[c] = c;
		}
		std::sort(SortedOrder.begin(), SortedOrder.end(), [&](int32 A, int32 B)
		{
			const float MeanA = Stats[A].Count > 0 ? (Stats[A].SumX / static_cast<float>(Stats[A].Count)) : 0.0f;
			const float MeanB = Stats[B].Count > 0 ? (Stats[B].SumX / static_cast<float>(Stats[B].Count)) : 0.0f;
			return MeanA < MeanB;
		});

		TArray<int32> CompToSlot;
		CompToSlot.SetNum(TileCount);
		for (int32 Slot = 0; Slot < TileCount; ++Slot)
		{
			CompToSlot[SortedOrder[Slot]] = Slot;
		}

		TArray<float> VertexLocalU;
		TArray<float> VertexLocalV;
		TArray<int32> VertexSlot;
		VertexLocalU.SetNum(PostSplitVertCount);
		VertexLocalV.SetNum(PostSplitVertCount);
		VertexSlot.SetNum(PostSplitVertCount);

		constexpr float TwoPi = 6.283185307f;
		constexpr float Pi = 3.1415926535f;

		for (int32 v = 0; v < PostSplitVertCount; ++v)
		{
			const int32 Tid = VertexTileId[v];
			const FCompStats& S = Stats[Tid];
			const float SpanZ = std::max(S.MaxZ - S.MinZ, 1e-4f);
			const float VCoord = std::clamp((InOutVertices[v].Position.Z - S.MinZ) / SpanZ, 0.0f, 1.0f);
			const float SliceF = VCoord * static_cast<float>(SliceCount - 1);
			const int32 SliceLo = std::clamp(static_cast<int32>(SliceF), 0, SliceCount - 1);
			const int32 SliceHi = std::min(SliceLo + 1, SliceCount - 1);
			const float Frac = SliceF - static_cast<float>(SliceLo);
			const float Cx = S.SliceCenterX[SliceLo] * (1.0f - Frac) + S.SliceCenterX[SliceHi] * Frac;
			const float Cy = S.SliceCenterY[SliceLo] * (1.0f - Frac) + S.SliceCenterY[SliceHi] * Frac;

			const float Angle = std::atan2(InOutVertices[v].Position.Y - Cy, InOutVertices[v].Position.X - Cx);
			VertexLocalU[v] = std::clamp((Angle + Pi) / TwoPi, 0.0f, 1.0f);
			VertexLocalV[v] = VCoord;
			VertexSlot[v] = CompToSlot[Tid];
		}

		// 원통 둘레 경계를 가로지르는 삼각형은 정점을 복제해 심 늘림 방지
		std::unordered_map<uint32, uint32> SeamDuplicates;
		for (int32 t = 0; t < TriCount; ++t)
		{
			const uint32 Idx0 = InOutIndices[t * 3];
			const uint32 Idx1 = InOutIndices[t * 3 + 1];
			const uint32 Idx2 = InOutIndices[t * 3 + 2];
			if (VertexSlot[static_cast<int32>(Idx0)] != VertexSlot[static_cast<int32>(Idx1)] ||
				VertexSlot[static_cast<int32>(Idx1)] != VertexSlot[static_cast<int32>(Idx2)])
			{
				continue;
			}
			const float U0 = VertexLocalU[static_cast<int32>(Idx0)];
			const float U1 = VertexLocalU[static_cast<int32>(Idx1)];
			const float U2 = VertexLocalU[static_cast<int32>(Idx2)];
			const float MinU = std::min(U0, std::min(U1, U2));
			const float MaxU = std::max(U0, std::max(U1, U2));
			if ((MaxU - MinU) <= 0.5f)
			{
				continue;
			}

			for (int32 c = 0; c < 3; ++c)
			{
				const uint32 OrigIdx = InOutIndices[t * 3 + c];
				if (VertexLocalU[static_cast<int32>(OrigIdx)] >= 0.5f)
				{
					continue;
				}
				const std::unordered_map<uint32, uint32>::const_iterator Found = SeamDuplicates.find(OrigIdx);
				if (Found != SeamDuplicates.end())
				{
					InOutIndices[t * 3 + c] = Found->second;
				}
				else
				{
					const uint32 NewIdx = static_cast<uint32>(InOutVertices.Num());
					InOutVertices.Add(InOutVertices[static_cast<int32>(OrigIdx)]);
					InOutWeights.Add(InOutWeights[static_cast<int32>(OrigIdx)]);
					VertexLocalU.Add(VertexLocalU[static_cast<int32>(OrigIdx)] + 1.0f);
					VertexLocalV.Add(VertexLocalV[static_cast<int32>(OrigIdx)]);
					VertexSlot.Add(VertexSlot[static_cast<int32>(OrigIdx)]);
					SeamDuplicates.emplace(OrigIdx, NewIdx);
					InOutIndices[t * 3 + c] = NewIdx;
				}
			}
		}

		const float TileSpan = 1.0f / static_cast<float>(TileCount);
		const float TileWidth = TileSpan * 0.90f;
		const float TileMargin = TileSpan * 0.02f;
		const int32 FinalVertCount = InOutVertices.Num();
		for (int32 v = 0; v < FinalVertCount; ++v)
		{
			const float TileOrigin = static_cast<float>(VertexSlot[v]) * TileSpan + TileMargin;
			const float AtlasU = std::clamp(TileOrigin + VertexLocalU[v] * TileWidth, 0.0f, 1.0f);
			InOutVertices[v].UV = FVector2(AtlasU, VertexLocalV[v]);
		}
	}

	// 용접 정점의 평균 노멀 방향으로 표면 오프셋을 미리 반영
	void ApplySurfaceOffsetToVertices(TArray<FVertex>& InOutVertices, float Offset)
	{
		if (Offset <= 1e-6f || InOutVertices.IsEmpty())
		{
			return;
		}

		TArray<uint32> WeldedIds;
		BuildWeldedVertexIds(InOutVertices, WeldedIds);
		const int32 VertCount = InOutVertices.Num();
		uint32 WeldedCount = 0;
		for (int32 v = 0; v < VertCount; ++v)
		{
			WeldedCount = std::max(WeldedCount, WeldedIds[v] + 1u);
		}

		TArray<FVector> SmoothNormals;
		SmoothNormals.SetNum(static_cast<int32>(WeldedCount));
		for (uint32 w = 0; w < WeldedCount; ++w)
		{
			SmoothNormals[static_cast<int32>(w)] = FVector::ZeroVector;
		}
		for (int32 v = 0; v < VertCount; ++v)
		{
			SmoothNormals[static_cast<int32>(WeldedIds[v])] += InOutVertices[v].Normal;
		}
		for (uint32 w = 0; w < WeldedCount; ++w)
		{
			FVector& N = SmoothNormals[static_cast<int32>(w)];
			if (N.SizeSquared() > 1e-10f)
			{
				N = N.Normalized();
			}
		}

		for (int32 v = 0; v < VertCount; ++v)
		{
			const FVector& N = SmoothNormals[static_cast<int32>(WeldedIds[v])];
			if (N.SizeSquared() > 1e-10f)
			{
				InOutVertices[v].Position += N * Offset;
			}
		}
	}
}

uint64 UStockingRegion::ComputeMeshTopologyHash(const USkeletalMesh& SourceMesh, int32 LODIndex)
{
	const FSkeletalMeshRenderData& RenderData = SourceMesh.GetRenderData();
	if (LODIndex < 0 || LODIndex >= RenderData.LODRenderData.Num())
	{
		return 0;
	}

	const FSkeletalMeshLODRenderData& LOD = RenderData.LODRenderData[LODIndex];
	uint64 Hash = 14695981039346656037ull;
	Hash = MixHashValue(Hash, static_cast<uint64>(LOD.BindVertices.Num()));
	Hash = MixHashValue(Hash, static_cast<uint64>(LOD.Indices.Num()));
	Hash = MixHashValue(Hash, static_cast<uint64>(SourceMesh.GetNumBones()));

	const int32 Step = std::max(1, LOD.Indices.Num() / 1024);
	for (int32 i = 0; i < LOD.Indices.Num(); i += Step)
	{
		Hash = MixHashValue(Hash, static_cast<uint64>(LOD.Indices[i]));
	}
	return Hash;
}

void UStockingRegion::BindToSourceMesh(const USkeletalMesh& SourceMesh, const FString& InSourceMeshPath, int32 InLODIndex)
{
	SourceMeshPath = InSourceMeshPath;
	SourceLODIndex = InLODIndex;

	const FSkeletalMeshRenderData& RenderData = SourceMesh.GetRenderData();
	if (InLODIndex >= 0 && InLODIndex < RenderData.LODRenderData.Num())
	{
		const FSkeletalMeshLODRenderData& LOD = RenderData.LODRenderData[InLODIndex];
		SourceVertexCount = static_cast<uint32>(LOD.BindVertices.Num());
		SourceTriangleCount = static_cast<uint32>(LOD.Indices.Num() / 3);
		SourceTopologyHash = ComputeMeshTopologyHash(SourceMesh, InLODIndex);
	}
	else
	{
		SourceVertexCount = 0;
		SourceTriangleCount = 0;
		SourceTopologyHash = 0;
	}
}

void UStockingRegion::AddTriangle(uint32 TriangleIndex)
{
	if (!ContainsTriangle(TriangleIndex))
	{
		TriangleIndices.Add(TriangleIndex);
		std::sort(TriangleIndices.begin(), TriangleIndices.end());
	}
}

void UStockingRegion::RemoveTriangle(uint32 TriangleIndex)
{
	const std::vector<uint32>::iterator It = std::lower_bound(TriangleIndices.begin(), TriangleIndices.end(), TriangleIndex);
	if (It != TriangleIndices.end() && *It == TriangleIndex)
	{
		const int32 Index = static_cast<int32>(It - TriangleIndices.begin());
		TriangleIndices.RemoveAt(Index, 1);
	}
}

void UStockingRegion::SetTriangles(const TArray<uint32>& InTriangles)
{
	TriangleIndices = InTriangles;
	std::sort(TriangleIndices.begin(), TriangleIndices.end());
	const std::vector<uint32>::iterator UniqueEnd = std::unique(TriangleIndices.begin(), TriangleIndices.end());
	TriangleIndices.SetNum(static_cast<int32>(UniqueEnd - TriangleIndices.begin()));
}

void UStockingRegion::ClearTriangles()
{
	TriangleIndices.Reset();
}

bool UStockingRegion::ContainsTriangle(uint32 TriangleIndex) const
{
	return std::binary_search(TriangleIndices.begin(), TriangleIndices.end(), TriangleIndex);
}

void UStockingRegion::SelectTrianglesByBones(const USkeletalMesh& SourceMesh, const TArray<int32>& BoneIndices, float MinWeightThreshold)
{
	const FSkeletalMeshRenderData& RenderData = SourceMesh.GetRenderData();
	if (SourceLODIndex < 0 || SourceLODIndex >= RenderData.LODRenderData.Num())
	{
		return;
	}

	const FSkeletalMeshLODRenderData& LOD = RenderData.LODRenderData[SourceLODIndex];
	if (LOD.SkinWeights.Num() != LOD.BindVertices.Num() || BoneIndices.IsEmpty())
	{
		return;
	}

	std::unordered_set<int32> TargetBones;
	for (int32 Bone : BoneIndices)
	{
		TargetBones.insert(Bone);
	}

	const int32 TriCount = LOD.Indices.Num() / 3;
	TArray<uint32> Selected = TriangleIndices;
	for (int32 t = 0; t < TriCount; ++t)
	{
		float TriWeightSum = 0.0f;
		for (int32 corner = 0; corner < 3; ++corner)
		{
			const uint32 VIdx = LOD.Indices[t * 3 + corner];
			if (static_cast<int32>(VIdx) >= LOD.SkinWeights.Num())
			{
				continue;
			}
			const FSkinWeight& W = LOD.SkinWeights[VIdx];
			for (int32 k = 0; k < MaxBoneInfluences; ++k)
			{
				if (W.Weight[k] > 0.0f && TargetBones.find(static_cast<int32>(W.BoneIndex[k])) != TargetBones.end())
				{
					TriWeightSum += W.Weight[k];
				}
			}
		}
		if ((TriWeightSum / 3.0f) >= MinWeightThreshold)
		{
			Selected.Add(static_cast<uint32>(t));
		}
	}

	SetTriangles(Selected);
}

void UStockingRegion::SelectTrianglesByBoneFilter(
	const USkeletalMesh& SourceMesh,
	const TArray<int32>& BoneIndices,
	float MinWeightThreshold,
	float MaxBoneRadiusRatio,
	float MaxLegHeightRatio)
{
	const FSkeletalMeshRenderData& RenderData = SourceMesh.GetRenderData();
	if (SourceLODIndex < 0 || SourceLODIndex >= RenderData.LODRenderData.Num())
	{
		return;
	}

	const FSkeletalMeshLODRenderData& LOD = RenderData.LODRenderData[SourceLODIndex];
	const TArray<FBoneInfo>& Bones = SourceMesh.GetRefSkeleton();
	if (LOD.SkinWeights.Num() != LOD.BindVertices.Num() || BoneIndices.IsEmpty() || Bones.IsEmpty())
	{
		return;
	}

	// 바인드 포즈 기준 본 위치 및 대상 본 세그먼트 계산
	TArray<FMatrix> ComponentTransforms;
	TArray<FVector> BonePositions;
	ComponentTransforms.SetNum(Bones.Num());
	BonePositions.SetNum(Bones.Num());
	for (int32 i = 0; i < Bones.Num(); ++i)
	{
		const FMatrix Local = Bones[i].LocalBindPose.GetLocalMatrix();
		const int32 Parent = Bones[i].ParentIndex;
		ComponentTransforms[i] = (Parent >= 0 && Parent < i) ? (Local * ComponentTransforms[Parent]) : Local;
		BonePositions[i] = ComponentTransforms[i].TransformPosition(FVector::ZeroVector);
	}

	std::unordered_set<int32> TargetBones;
	float LegMaxZ = -FLT_MAX;
	for (int32 Bone : BoneIndices)
	{
		if (Bone >= 0 && Bone < Bones.Num())
		{
			TargetBones.insert(Bone);
			LegMaxZ = std::max(LegMaxZ, BonePositions[Bone].Z);
		}
	}
	if (TargetBones.empty())
	{
		return;
	}

	struct FBoneSegment
	{
		FVector Start;
		FVector End;
		FVector Axis;
		FVector BasisU;
		FVector BasisV;
		float Length = 0.0f;
	};
	const std::function<FBoneSegment(const FVector&, const FVector&)> MakeBoneSegment =
		[](const FVector& StartPos, const FVector& EndPos) -> FBoneSegment
	{
		FBoneSegment Seg{};
		Seg.Start = StartPos;
		Seg.End = EndPos;
		const FVector Dir = EndPos - StartPos;
		Seg.Length = Dir.Size();
		if (Seg.Length > 1e-5f)
		{
			Seg.Axis = Dir / Seg.Length;
			const FVector RefUp = std::fabs(Seg.Axis.Z) < 0.9f
				? FVector(0.0f, 0.0f, 1.0f)
				: FVector(1.0f, 0.0f, 0.0f);
			Seg.BasisU = FVector::Cross(RefUp, Seg.Axis).Normalized();
			Seg.BasisV = FVector::Cross(Seg.Axis, Seg.BasisU).Normalized();
		}
		else
		{
			Seg.Axis = FVector(0.0f, 0.0f, 1.0f);
			Seg.BasisU = FVector(1.0f, 0.0f, 0.0f);
			Seg.BasisV = FVector(0.0f, 1.0f, 0.0f);
		}
		return Seg;
	};

	TArray<FBoneSegment> Segments;
	for (int32 Bone : BoneIndices)
	{
		if (Bone < 0 || Bone >= Bones.Num())
		{
			continue;
		}
		bool bAddedChild = false;
		for (int32 c = 0; c < Bones.Num(); ++c)
		{
			if (Bones[c].ParentIndex == Bone)
			{
				Segments.Add(MakeBoneSegment(BonePositions[Bone], BonePositions[c]));
				bAddedChild = true;
			}
		}
		if (!bAddedChild)
		{
			Segments.Add(MakeBoneSegment(BonePositions[Bone], BonePositions[Bone]));
		}
	}

	float MeshMinZ = FLT_MAX;
	float MeshMaxZ = -FLT_MAX;
	for (int32 v = 0; v < LOD.BindVertices.Num(); ++v)
	{
		const float Z = LOD.BindVertices[v].Position.Z;
		MeshMinZ = std::min(MeshMinZ, Z);
		MeshMaxZ = std::max(MeshMaxZ, Z);
	}
	const float MeshHeight = std::max(MeshMaxZ - MeshMinZ, 1e-4f);
	const float LegSpanZ = std::max(LegMaxZ - MeshMinZ, MeshHeight * 0.45f);
	const float MaxAllowedZ = MeshMinZ + LegSpanZ * MaxLegHeightRatio;
	const float MaxAllowedRadius = MeshHeight * MaxBoneRadiusRatio;
	const float MaxAllowedRadiusSq = MaxAllowedRadius * MaxAllowedRadius;
	const float FootExemptZ = MeshMinZ + LegSpanZ * 0.14f;
	const float LowerLegSeedZ = MeshMinZ + LegSpanZ * 0.34f;
	const float MinCylSegmentLen = MeshHeight * 0.05f;
	const float InnerShellMargin = MeshHeight * 0.014f;

	constexpr int32 NumSlices = 20;
	constexpr int32 NumAngles = 24;
	const int32 BinsPerSeg = NumSlices * NumAngles;
	TArray<float> MinRadiusGrid;
	MinRadiusGrid.SetNum(Segments.Num() * BinsPerSeg);
	for (int32 i = 0; i < MinRadiusGrid.Num(); ++i)
	{
		MinRadiusGrid[i] = FLT_MAX;
	}

	struct FCandidateTri
	{
		uint32 TriIndex = 0;
		int32 SegIndex = -1;
		int32 GridBin = -1;
		float RadialDist = 0.0f;
		float CenterZ = 0.0f;
	};

	const int32 TriCount = LOD.Indices.Num() / 3;
	TArray<FCandidateTri> Candidates;
	Candidates.Reserve(TriCount / 4);

	for (int32 t = 0; t < TriCount; ++t)
	{
		const uint32 I0 = LOD.Indices[t * 3];
		const uint32 I1 = LOD.Indices[t * 3 + 1];
		const uint32 I2 = LOD.Indices[t * 3 + 2];

		float TriWeightSum = 0.0f;
		const uint32 CornerIdx[3] = { I0, I1, I2 };
		for (int32 corner = 0; corner < 3; ++corner)
		{
			const uint32 VIdx = CornerIdx[corner];
			if (static_cast<int32>(VIdx) >= LOD.SkinWeights.Num())
			{
				continue;
			}
			const FSkinWeight& W = LOD.SkinWeights[VIdx];
			for (int32 k = 0; k < MaxBoneInfluences; ++k)
			{
				if (W.Weight[k] > 0.0f && TargetBones.find(static_cast<int32>(W.BoneIndex[k])) != TargetBones.end())
				{
					TriWeightSum += W.Weight[k];
				}
			}
		}

		if ((TriWeightSum / 3.0f) < MinWeightThreshold)
		{
			continue;
		}

		const FVector& P0 = LOD.BindVertices[I0].Position;
		const FVector& P1 = LOD.BindVertices[I1].Position;
		const FVector& P2 = LOD.BindVertices[I2].Position;
		const FVector Center = (P0 + P1 + P2) * (1.0f / 3.0f);

		// 상단 높이 컷오프로 허리 및 치마 상단 제외
		if (MaxLegHeightRatio > 0.0f && Center.Z > MaxAllowedZ)
		{
			continue;
		}

		FCandidateTri Cand{};
		Cand.TriIndex = static_cast<uint32>(t);
		Cand.CenterZ = Center.Z;

		// 다리 본 세그먼트 반경 필터로 바깥쪽 치마 폴리곤 제외
		if (MaxBoneRadiusRatio > 0.0f && !Segments.IsEmpty())
		{
			float BestDistSq = FLT_MAX;
			FVector BestClosest = Segments[0].Start;
			int32 BestSeg = 0;
			float BestT = 0.0f;
			for (int32 s = 0; s < Segments.Num(); ++s)
			{
				const FVector SegDir = Segments[s].End - Segments[s].Start;
				const float SegLenSq = SegDir.SizeSquared();
				const float T = SegLenSq > 1e-10f
					? FMath::Clamp((Center - Segments[s].Start).Dot(SegDir) / SegLenSq, 0.0f, 1.0f)
					: 0.0f;
				const FVector Closest = Segments[s].Start + SegDir * T;
				const float DistSq = (Center - Closest).SizeSquared();
				if (DistSq < BestDistSq)
				{
					BestDistSq = DistSq;
					BestClosest = Closest;
					BestSeg = s;
					BestT = T;
				}
			}

			if (BestDistSq > MaxAllowedRadiusSq)
			{
				continue;
			}

			// 본 축을 향해 안쪽으로 뒤집힌 치마 내면 제외
			const FVector Radial = Center - BestClosest;
			const float RadialLenSq = Radial.SizeSquared();
			const float RadialLen = RadialLenSq > 1e-10f ? std::sqrt(RadialLenSq) : 0.0f;
			if (RadialLen > 1e-5f)
			{
				const FVector AvgNormal = LOD.BindVertices[I0].Normal + LOD.BindVertices[I1].Normal + LOD.BindVertices[I2].Normal;
				if (AvgNormal.SizeSquared() > 1e-8f)
				{
					const float RadialDot = AvgNormal.Normalized().Dot(Radial / RadialLen);
					if (RadialDot < -0.45f)
					{
						continue;
					}
				}
			}

			Cand.SegIndex = BestSeg;
			Cand.RadialDist = RadialLen;

			// 발 구역을 제외한 원통형 다리 세그먼트의 각도별 최소 반경 기록
			if (Center.Z > FootExemptZ && Segments[BestSeg].Length > MinCylSegmentLen && RadialLen > 1e-5f)
			{
				const float U = Radial.Dot(Segments[BestSeg].BasisU);
				const float V = Radial.Dot(Segments[BestSeg].BasisV);
				const float AngleNorm = (std::atan2(V, U) + 3.14159265f) / 6.28318530f;
				const int32 AngleBin = FMath::Clamp(static_cast<int32>(AngleNorm * static_cast<float>(NumAngles)), 0, NumAngles - 1);
				const int32 SliceBin = FMath::Clamp(static_cast<int32>(BestT * static_cast<float>(NumSlices)), 0, NumSlices - 1);
				const int32 GridBin = BestSeg * BinsPerSeg + SliceBin * NumAngles + AngleBin;
				Cand.GridBin = GridBin;
				if (RadialLen < MinRadiusGrid[GridBin])
				{
					MinRadiusGrid[GridBin] = RadialLen;
				}
			}
		}

		Candidates.Add(Cand);
	}

	// 다층 레이어 중 본에서 가장 안쪽 피부 껍질만 유지해 바깥 치마 외피 제거
	TArray<FCandidateTri> ShellFiltered;
	ShellFiltered.Reserve(Candidates.Num());
	for (int32 i = 0; i < Candidates.Num(); ++i)
	{
		const FCandidateTri& Cand = Candidates[i];
		if (Cand.GridBin >= 0)
		{
			const float MinR = MinRadiusGrid[Cand.GridBin];
			if (MinR < FLT_MAX * 0.5f && Cand.RadialDist > MinR + InnerShellMargin)
			{
				continue;
			}
		}
		ShellFiltered.Add(Cand);
	}

	// 발목과 종아리 시드에서 용접 에지로 연결된 다리 성분만 남겨 치마 조각 제거
	TArray<uint32> FinalRegionTris;
	if (!ShellFiltered.IsEmpty())
	{
		TArray<uint32> WeldedIds;
		BuildWeldedVertexIds(LOD.BindVertices, WeldedIds);

		std::unordered_map<uint32, TArray<int32>> WeldedToCand;
		WeldedToCand.reserve(static_cast<size_t>(ShellFiltered.Num()));
		TArray<int32> BfsQueue;
		TArray<uint8> Visited;
		Visited.SetNum(ShellFiltered.Num());
		for (int32 i = 0; i < Visited.Num(); ++i)
		{
			Visited[i] = 0;
		}

		for (int32 i = 0; i < ShellFiltered.Num(); ++i)
		{
			const uint32 Tri = ShellFiltered[i].TriIndex;
			WeldedToCand[WeldedIds[LOD.Indices[Tri * 3]]].Add(i);
			WeldedToCand[WeldedIds[LOD.Indices[Tri * 3 + 1]]].Add(i);
			WeldedToCand[WeldedIds[LOD.Indices[Tri * 3 + 2]]].Add(i);

			if (ShellFiltered[i].CenterZ <= LowerLegSeedZ)
			{
				Visited[i] = 1;
				BfsQueue.Add(i);
			}
		}

		if (!BfsQueue.IsEmpty())
		{
			int32 Head = 0;
			while (Head < BfsQueue.Num())
			{
				const int32 Cur = BfsQueue[Head++];
				const uint32 Tri = ShellFiltered[Cur].TriIndex;
				const uint32 TriWelds[3] = {
					WeldedIds[LOD.Indices[Tri * 3]],
					WeldedIds[LOD.Indices[Tri * 3 + 1]],
					WeldedIds[LOD.Indices[Tri * 3 + 2]]
				};
				for (int32 k = 0; k < 3; ++k)
				{
					const std::unordered_map<uint32, TArray<int32>>::const_iterator Found = WeldedToCand.find(TriWelds[k]);
					if (Found == WeldedToCand.end())
					{
						continue;
					}
					const TArray<int32>& Neighbors = Found->second;
					for (int32 n = 0; n < Neighbors.Num(); ++n)
					{
						const int32 Next = Neighbors[n];
						if (Visited[Next] == 0)
						{
							Visited[Next] = 1;
							BfsQueue.Add(Next);
						}
					}
				}
			}

			FinalRegionTris.Reserve(BfsQueue.Num());
			for (int32 i = 0; i < BfsQueue.Num(); ++i)
			{
				FinalRegionTris.Add(ShellFiltered[BfsQueue[i]].TriIndex);
			}
		}
		else
		{
			FinalRegionTris.Reserve(ShellFiltered.Num());
			for (int32 i = 0; i < ShellFiltered.Num(); ++i)
			{
				FinalRegionTris.Add(ShellFiltered[i].TriIndex);
			}
		}

		// 스타킹 내부 구멍과 상단 경계 V자 틈을 용접 에지 기준으로 봉합
		if (!FinalRegionTris.IsEmpty())
		{
			TArray<uint8> SelectedMask;
			SelectedMask.SetNum(TriCount);
			for (int32 t = 0; t < TriCount; ++t)
			{
				SelectedMask[t] = 0;
			}
			for (int32 i = 0; i < FinalRegionTris.Num(); ++i)
			{
				SelectedMask[static_cast<int32>(FinalRegionTris[i])] = 1;
			}

			// 용접 에지별 인접 삼각형 테이블과 컷오프 여부 및 다리 피부 후보 판별 테이블 구성
			std::unordered_map<uint64, TArray<int32>> EdgeToTris;
			EdgeToTris.reserve(static_cast<size_t>(TriCount * 2));
			TArray<uint8> TriAboveCut;
			TArray<uint8> TriNearLegSkin;
			TriAboveCut.SetNum(TriCount);
			TriNearLegSkin.SetNum(TriCount);

			for (int32 t = 0; t < TriCount; ++t)
			{
				const uint32 I0 = LOD.Indices[t * 3];
				const uint32 I1 = LOD.Indices[t * 3 + 1];
				const uint32 I2 = LOD.Indices[t * 3 + 2];
				const FVector Center = (LOD.BindVertices[I0].Position +
					LOD.BindVertices[I1].Position +
					LOD.BindVertices[I2].Position) * (1.0f / 3.0f);
				TriAboveCut[t] = (MaxLegHeightRatio > 0.0f && Center.Z > MaxAllowedZ) ? 1 : 0;

				bool bNearLeg = true;
				if (MaxBoneRadiusRatio > 0.0f && !Segments.IsEmpty())
				{
					float BestDistSq = FLT_MAX;
					FVector BestClosest = Segments[0].Start;
					for (int32 s = 0; s < Segments.Num(); ++s)
					{
						const FVector SegDir = Segments[s].End - Segments[s].Start;
						const float SegLenSq = SegDir.SizeSquared();
						const float T = SegLenSq > 1e-10f
							? FMath::Clamp((Center - Segments[s].Start).Dot(SegDir) / SegLenSq, 0.0f, 1.0f)
							: 0.0f;
						const FVector Closest = Segments[s].Start + SegDir * T;
						const float DistSq = (Center - Closest).SizeSquared();
						if (DistSq < BestDistSq)
						{
							BestDistSq = DistSq;
							BestClosest = Closest;
						}
					}
					if (BestDistSq > MaxAllowedRadiusSq)
					{
						bNearLeg = false;
					}
					else
					{
						const FVector Radial = Center - BestClosest;
						const float RadialLenSq = Radial.SizeSquared();
						if (RadialLenSq > 1e-10f)
						{
							const FVector AvgNormal = LOD.BindVertices[I0].Normal + LOD.BindVertices[I1].Normal + LOD.BindVertices[I2].Normal;
							if (AvgNormal.SizeSquared() > 1e-8f)
							{
								const float RadialDot = AvgNormal.Normalized().Dot(Radial / std::sqrt(RadialLenSq));
								if (RadialDot < -0.45f)
								{
									bNearLeg = false;
								}
							}
						}
					}
				}
				TriNearLegSkin[t] = bNearLeg ? 1 : 0;

				const uint32 W[3] = { WeldedIds[I0], WeldedIds[I1], WeldedIds[I2] };
				for (int32 e = 0; e < 3; ++e)
				{
					if (W[e] != W[(e + 1) % 3])
					{
						EdgeToTris[MakeEdgeKey(W[e], W[(e + 1) % 3])].Add(t);
					}
				}
			}

			// 선택된 삼각형과 에지를 공유하는 소규모 닫힌 구멍을 에지 기반으로 탐색해 봉합
			TArray<uint8> HoleState;
			HoleState.SetNum(TriCount);
			for (int32 t = 0; t < TriCount; ++t)
			{
				HoleState[t] = SelectedMask[t];
			}

			const int32 MaxHoleTris = std::max(250, FinalRegionTris.Num() / 10);
			TArray<int32> SeedTris;
			for (int32 i = 0; i < FinalRegionTris.Num(); ++i)
			{
				const uint32 SelTri = FinalRegionTris[i];
				const uint32 W[3] = {
					WeldedIds[LOD.Indices[SelTri * 3]],
					WeldedIds[LOD.Indices[SelTri * 3 + 1]],
					WeldedIds[LOD.Indices[SelTri * 3 + 2]]
				};
				for (int32 e = 0; e < 3; ++e)
				{
					if (W[e] == W[(e + 1) % 3])
					{
						continue;
					}
					const std::unordered_map<uint64, TArray<int32>>::const_iterator Found =
						EdgeToTris.find(MakeEdgeKey(W[e], W[(e + 1) % 3]));
					if (Found == EdgeToTris.end())
					{
						continue;
					}
					const TArray<int32>& Neighbors = Found->second;
					for (int32 n = 0; n < Neighbors.Num(); ++n)
					{
						const int32 CandTri = Neighbors[n];
						if (HoleState[CandTri] == 0 && TriAboveCut[CandTri] == 0 && TriNearLegSkin[CandTri] != 0)
						{
							SeedTris.Add(CandTri);
						}
					}
				}
			}

			TArray<int32> HoleComp;
			for (int32 s = 0; s < SeedTris.Num(); ++s)
			{
				const int32 StartTri = SeedTris[s];
				if (HoleState[StartTri] != 0)
				{
					continue;
				}

				HoleComp.Reset();
				HoleState[StartTri] = 2;
				HoleComp.Add(StartTri);
				bool bEscaped = (TriAboveCut[StartTri] != 0 || TriNearLegSkin[StartTri] == 0);

				int32 Head = 0;
				while (Head < HoleComp.Num() && !bEscaped)
				{
					const int32 Cur = HoleComp[Head++];
					if (TriAboveCut[Cur] != 0 || TriNearLegSkin[Cur] == 0 || HoleComp.Num() > MaxHoleTris)
					{
						bEscaped = true;
						break;
					}

					const uint32 W[3] = {
						WeldedIds[LOD.Indices[Cur * 3]],
						WeldedIds[LOD.Indices[Cur * 3 + 1]],
						WeldedIds[LOD.Indices[Cur * 3 + 2]]
					};
					for (int32 e = 0; e < 3 && !bEscaped; ++e)
					{
						if (W[e] == W[(e + 1) % 3])
						{
							continue;
						}
						const std::unordered_map<uint64, TArray<int32>>::const_iterator Found =
							EdgeToTris.find(MakeEdgeKey(W[e], W[(e + 1) % 3]));
						if (Found == EdgeToTris.end())
						{
							continue;
						}
						const TArray<int32>& Neighbors = Found->second;
						for (int32 n = 0; n < Neighbors.Num(); ++n)
						{
							const int32 Next = Neighbors[n];
							if (HoleState[Next] == 3 || TriAboveCut[Next] != 0 || TriNearLegSkin[Next] == 0)
							{
								bEscaped = true;
								break;
							}
							if (HoleState[Next] == 0)
							{
								HoleState[Next] = 2;
								HoleComp.Add(Next);
								if (HoleComp.Num() > MaxHoleTris)
								{
									bEscaped = true;
									break;
								}
							}
						}
					}
				}

				if (bEscaped)
				{
					for (int32 h = 0; h < HoleComp.Num(); ++h)
					{
						HoleState[HoleComp[h]] = 3;
					}
				}
				else
				{
					for (int32 h = 0; h < HoleComp.Num(); ++h)
					{
						HoleState[HoleComp[h]] = 1;
						SelectedMask[HoleComp[h]] = 1;
						FinalRegionTris.Add(static_cast<uint32>(HoleComp[h]));
					}
				}
			}

			// 선택된 삼각형과 두 변 이상 맞닿은 피부 삼각형을 반복 봉합해 세로 틈과 다이아몬드 구멍 제거
			constexpr int32 NotchSealPasses = 4;
			for (int32 pass = 0; pass < NotchSealPasses; ++pass)
			{
				TArray<int32> NotchFillTris;
				for (int32 t = 0; t < TriCount; ++t)
				{
					if (SelectedMask[t] != 0 || TriNearLegSkin[t] == 0)
					{
						continue;
					}

					const uint32 W[3] = {
						WeldedIds[LOD.Indices[t * 3]],
						WeldedIds[LOD.Indices[t * 3 + 1]],
						WeldedIds[LOD.Indices[t * 3 + 2]]
					};
					int32 SharedSelectedEdges = 0;
					for (int32 e = 0; e < 3; ++e)
					{
						if (W[e] == W[(e + 1) % 3])
						{
							continue;
						}
						const std::unordered_map<uint64, TArray<int32>>::const_iterator Found =
							EdgeToTris.find(MakeEdgeKey(W[e], W[(e + 1) % 3]));
						if (Found == EdgeToTris.end())
						{
							continue;
						}
						const TArray<int32>& Neighbors = Found->second;
						for (int32 n = 0; n < Neighbors.Num(); ++n)
						{
							if (Neighbors[n] != t && SelectedMask[Neighbors[n]] != 0)
							{
								++SharedSelectedEdges;
								break;
							}
						}
					}

					if (SharedSelectedEdges >= 2)
					{
						NotchFillTris.Add(t);
					}
				}

				if (NotchFillTris.IsEmpty())
				{
					break;
				}
				for (int32 i = 0; i < NotchFillTris.Num(); ++i)
				{
					SelectedMask[NotchFillTris[i]] = 1;
					FinalRegionTris.Add(static_cast<uint32>(NotchFillTris[i]));
				}
			}
		}
	}

	TArray<uint32> Selected = TriangleIndices;
	for (int32 i = 0; i < FinalRegionTris.Num(); ++i)
	{
		Selected.Add(FinalRegionTris[i]);
	}

	SetTriangles(Selected);
}

float UStockingRegion::ComputeReferenceScale(const USkeletalMesh& SourceMesh, int32 LODIndex)
{
	const FSkeletalMeshRenderData& RenderData = SourceMesh.GetRenderData();
	if (LODIndex < 0 || LODIndex >= RenderData.LODRenderData.Num())
	{
		return 1.0f;
	}
	return ComputeVerticesReferenceScale(RenderData.LODRenderData[LODIndex].BindVertices);
}

void UStockingRegion::GrowSelection(const USkeletalMesh& SourceMesh)
{
	const FSkeletalMeshRenderData& RenderData = SourceMesh.GetRenderData();
	if (SourceLODIndex < 0 || SourceLODIndex >= RenderData.LODRenderData.Num() || TriangleIndices.IsEmpty())
	{
		return;
	}

	const FSkeletalMeshLODRenderData& LOD = RenderData.LODRenderData[SourceLODIndex];
	const int32 TriCount = LOD.Indices.Num() / 3;

	TArray<uint32> WeldedIds;
	BuildWeldedVertexIds(LOD.BindVertices, WeldedIds);

	std::unordered_set<uint32> SelectedVertices;
	for (uint32 Tri : TriangleIndices)
	{
		if (static_cast<int32>(Tri) >= TriCount)
		{
			continue;
		}
		SelectedVertices.insert(WeldedIds[LOD.Indices[Tri * 3]]);
		SelectedVertices.insert(WeldedIds[LOD.Indices[Tri * 3 + 1]]);
		SelectedVertices.insert(WeldedIds[LOD.Indices[Tri * 3 + 2]]);
	}

	TArray<uint32> Expanded = TriangleIndices;
	for (int32 t = 0; t < TriCount; ++t)
	{
		if (SelectedVertices.contains(WeldedIds[LOD.Indices[t * 3]]) ||
			SelectedVertices.contains(WeldedIds[LOD.Indices[t * 3 + 1]]) ||
			SelectedVertices.contains(WeldedIds[LOD.Indices[t * 3 + 2]]))
		{
			Expanded.Add(static_cast<uint32>(t));
		}
	}
	SetTriangles(Expanded);
}

void UStockingRegion::ShrinkSelection(const USkeletalMesh& SourceMesh)
{
	const FSkeletalMeshRenderData& RenderData = SourceMesh.GetRenderData();
	if (SourceLODIndex < 0 || SourceLODIndex >= RenderData.LODRenderData.Num() || TriangleIndices.IsEmpty())
	{
		return;
	}

	const FSkeletalMeshLODRenderData& LOD = RenderData.LODRenderData[SourceLODIndex];
	const int32 TriCount = LOD.Indices.Num() / 3;

	TArray<uint32> WeldedIds;
	BuildWeldedVertexIds(LOD.BindVertices, WeldedIds);

	std::unordered_set<uint32> UnselectedVertices;
	for (int32 t = 0; t < TriCount; ++t)
	{
		if (ContainsTriangle(static_cast<uint32>(t)))
		{
			continue;
		}
		UnselectedVertices.insert(WeldedIds[LOD.Indices[t * 3]]);
		UnselectedVertices.insert(WeldedIds[LOD.Indices[t * 3 + 1]]);
		UnselectedVertices.insert(WeldedIds[LOD.Indices[t * 3 + 2]]);
	}

	TArray<uint32> Shrunk;
	for (uint32 Tri : TriangleIndices)
	{
		if (static_cast<int32>(Tri) >= TriCount)
		{
			continue;
		}
		if (!UnselectedVertices.contains(WeldedIds[LOD.Indices[Tri * 3]]) &&
			!UnselectedVertices.contains(WeldedIds[LOD.Indices[Tri * 3 + 1]]) &&
			!UnselectedVertices.contains(WeldedIds[LOD.Indices[Tri * 3 + 2]]))
		{
			Shrunk.Add(Tri);
		}
	}
	SetTriangles(Shrunk);
}

bool UStockingRegion::ValidateAgainstMesh(const USkeletalMesh& SourceMesh, FString& OutError) const
{
	OutError.clear();

	const FSkeletalMeshRenderData& RenderData = SourceMesh.GetRenderData();
	if (SourceLODIndex < 0 || SourceLODIndex >= RenderData.LODRenderData.Num())
	{
		OutError = "StockingRegion: source mesh LOD is missing.";
		return false;
	}

	const FSkeletalMeshLODRenderData& LOD = RenderData.LODRenderData[SourceLODIndex];
	const uint32 CurrentVertexCount = static_cast<uint32>(LOD.BindVertices.Num());
	const uint32 CurrentTriangleCount = static_cast<uint32>(LOD.Indices.Num() / 3);

	if (CurrentVertexCount == 0 || CurrentTriangleCount == 0)
	{
		OutError = "StockingRegion: source mesh has empty geometry.";
		return false;
	}

	if (SourceMesh.GetRefSkeleton().IsEmpty())
	{
		OutError = "StockingRegion: source mesh has no reference skeleton.";
		return false;
	}

	if (SourceVertexCount > 0 && SourceVertexCount != CurrentVertexCount)
	{
		OutError = "StockingRegion: source mesh vertex count changed after reimport.";
		return false;
	}

	if (SourceTriangleCount > 0 && SourceTriangleCount != CurrentTriangleCount)
	{
		OutError = "StockingRegion: source mesh triangle count changed after reimport.";
		return false;
	}

	const uint64 CurrentHash = ComputeMeshTopologyHash(SourceMesh, SourceLODIndex);
	if (SourceTopologyHash != 0 && SourceTopologyHash != CurrentHash)
	{
		OutError = "StockingRegion: source mesh topology hash mismatch after reimport.";
		return false;
	}

	if (TriangleIndices.IsEmpty())
	{
		OutError = "StockingRegion: no triangles selected.";
		return false;
	}

	for (int32 i = 0; i < TriangleIndices.Num(); ++i)
	{
		if (TriangleIndices[i] >= CurrentTriangleCount)
		{
			OutError = "StockingRegion: triangle index is out of range for source mesh.";
			return false;
		}
	}

	return true;
}

bool UStockingRegion::BuildStockingMesh(const USkeletalMesh& SourceMesh, USkeletalMesh& OutStockingMesh, FString& OutError) const
{
	if (!ValidateAgainstMesh(SourceMesh, OutError))
	{
		return false;
	}

	const FSkeletalMeshLODRenderData& SourceLOD = SourceMesh.GetRenderData().LODRenderData[SourceLODIndex];
	if (SourceLOD.SkinWeights.Num() != SourceLOD.BindVertices.Num())
	{
		OutError = "StockingRegion: source mesh skin weights do not match vertex count.";
		return false;
	}

	TArray<FVertex> SubVertices;
	TArray<FSkinWeight> SubWeights;
	TArray<uint32> SubIndices;
	SubIndices.Reserve(TriangleIndices.Num() * 3);

	std::unordered_map<uint32, uint32> SourceToLocalVertex;
	SourceToLocalVertex.reserve(static_cast<size_t>(TriangleIndices.Num()) * 2);

	for (int32 i = 0; i < TriangleIndices.Num(); ++i)
	{
		const uint32 TriIdx = TriangleIndices[i];
		for (int32 c = 0; c < 3; ++c)
		{
			const uint32 SrcV = SourceLOD.Indices[TriIdx * 3 + c];
			const std::unordered_map<uint32, uint32>::const_iterator Found = SourceToLocalVertex.find(SrcV);
			if (Found != SourceToLocalVertex.end())
			{
				SubIndices.Add(Found->second);
			}
			else
			{
				const uint32 NewV = static_cast<uint32>(SubVertices.Num());
				SubVertices.Add(SourceLOD.BindVertices[SrcV]);
				SubWeights.Add(SourceLOD.SkinWeights[SrcV]);
				SourceToLocalVertex.emplace(SrcV, NewV);
				SubIndices.Add(NewV);
			}
		}
	}

	// 필요 시 중점 세분화로 정점 밀도 보강
	const int32 ClampedSubdiv = std::clamp(SubdivisionIterations, 0, 2);
	for (int32 Iter = 0; Iter < ClampedSubdiv; ++Iter)
	{
		SubdivideTrianglesOnce(SubVertices, SubWeights, SubIndices);
	}
	if (ClampedSubdiv > 0)
	{
		const float SourceRefScale = ComputeVerticesReferenceScale(SourceLOD.BindVertices);
		const float TargetEdgeLen = (ClampedSubdiv >= 2 ? 0.0075f : 0.011f) * SourceRefScale;
		SubdivideLongEdgesAdaptive(SubVertices, SubWeights, SubIndices, TargetEdgeLen, 3, 28000);
	}

	WriteStockingVertexMasks(SubVertices, SubIndices, BoundaryFeatherDistance);
	GenerateStockingAtlasUVs(SubVertices, SubWeights, SubIndices);
	ApplySurfaceOffsetToVertices(SubVertices, SurfaceOffset);

	FSkeletalMeshData MeshData;
	MeshData.BindVertices = std::move(SubVertices);
	MeshData.SkinWeight = std::move(SubWeights);
	MeshData.Indices = std::move(SubIndices);
	MeshData.Bones = SourceMesh.GetRefSkeleton();
	MeshData.SourcePath = SourceMeshPath;

	FSkeletalMaterialSlot Slot;
	Slot.Name = "StockingMaterial";
	Slot.BaseColor = FVector4(0.08f, 0.07f, 0.09f, 0.65f);
	MeshData.MaterialSlots.Add(Slot);

	FSkeletalMeshSection Section;
	Section.StartIndex = 0;
	Section.IndexCount = static_cast<uint32>(MeshData.Indices.Num());
	Section.MaterialSlotIndex = 0;
	MeshData.Sections.Add(Section);

	return OutStockingMesh.Build(std::move(MeshData), OutError);
}

bool UStockingRegion::SaveToFile(const FString& FilePath) const
{
	if (FilePath.empty())
	{
		return false;
	}

	json Root = json::object();
	Root["Version"] = 2;
	Root["SourceMeshPath"] = SourceMeshPath;
	Root["SourceLODIndex"] = SourceLODIndex;
	Root["SourceVertexCount"] = SourceVertexCount;
	Root["SourceTriangleCount"] = SourceTriangleCount;
	Root["SourceTopologyHash"] = SourceTopologyHash;
	Root["SubdivisionIterations"] = SubdivisionIterations;
	Root["SurfaceOffset"] = SurfaceOffset;
	Root["BoundaryFeatherDistance"] = BoundaryFeatherDistance;

	if (bHasSavedMaterialParams)
	{
		json Mat = json::object();
		Mat["FabricColor"] = { SavedMaterialParams.FabricColor.X, SavedMaterialParams.FabricColor.Y, SavedMaterialParams.FabricColor.Z, SavedMaterialParams.FabricColor.W };
		Mat["HemColor"] = { SavedMaterialParams.HemColor.X, SavedMaterialParams.HemColor.Y, SavedMaterialParams.HemColor.Z, SavedMaterialParams.HemColor.W };
		Mat["BaseOpacity"] = SavedMaterialParams.BaseOpacity;
		Mat["FresnelOpacity"] = SavedMaterialParams.FresnelOpacity;
		Mat["FresnelPower"] = SavedMaterialParams.FresnelPower;
		Mat["StretchSensitivity"] = SavedMaterialParams.StretchSensitivity;
		Mat["StretchAlphaDrop"] = SavedMaterialParams.StretchAlphaDrop;
		Mat["MinOpacity"] = SavedMaterialParams.MinOpacity;
		Mat["WeaveScale"] = SavedMaterialParams.WeaveScale;
		Mat["WeaveStrength"] = SavedMaterialParams.WeaveStrength;
		Mat["HemFeatherPower"] = SavedMaterialParams.HemFeatherPower;
		Mat["HemBandStrength"] = SavedMaterialParams.HemBandStrength;
		Mat["SheenStrength"] = SavedMaterialParams.SheenStrength;
		Mat["SpecularIntensity"] = SavedMaterialParams.SpecularIntensity;
		Mat["SpecularPower"] = SavedMaterialParams.SpecularPower;
		Mat["WrinkleShadingStrength"] = SavedMaterialParams.WrinkleShadingStrength;
		Root["MaterialParams"] = std::move(Mat);
	}

	if (bHasSavedPullParams)
	{
		json Pull = json::object();
		Pull["PullRadius"] = SavedPullRadius;
		Pull["MaxPullDistance"] = SavedMaxPullDistance;
		Pull["PullFalloffExponent"] = SavedPullFalloffExponent;
		Pull["WrinkleAmplitude"] = SavedWrinkleAmplitude;
		Pull["WrinkleFrequency"] = SavedWrinkleFrequency;
		Pull["SpringStiffness"] = SavedSpringStiffness;
		Pull["SpringDamping"] = SavedSpringDamping;
		Pull["BoundsPadding"] = SavedBoundsPadding;
		Root["PullParams"] = std::move(Pull);
	}

	if (bHasSavedLegFilter)
	{
		json Filter = json::object();
		Filter["LegRadiusRatio"] = SavedLegRadiusRatio;
		Filter["LegHeightRatio"] = SavedLegHeightRatio;
		Root["LegFilter"] = std::move(Filter);
	}

	if (bHasSavedClothWrinkleParams)
	{
		json Cloth = json::object();
		Cloth["UseNvClothSimulation"] = SavedUseNvClothSimulation;
		Cloth["JointWrinkleSlack"] = SavedClothJointWrinkleSlack;
		Cloth["BackstopRadius"] = SavedClothBackstopRadius;
		Cloth["StretchStiffness"] = SavedClothStretchStiffness;
		Cloth["BendStiffness"] = SavedClothBendStiffness;
		Cloth["ShearStiffness"] = SavedClothShearStiffness;
		Cloth["CompressionLimit"] = SavedClothCompressionLimit;
		Cloth["StretchLimit"] = SavedClothStretchLimit;
		Cloth["MotionStiffness"] = SavedClothMotionStiffness;
		Cloth["Damping"] = SavedClothDamping;
		Cloth["SelfCollisionDistance"] = SavedClothSelfCollisionDistance;
		Cloth["SelfCollisionStiffness"] = SavedClothSelfCollisionStiffness;
		Root["ClothWrinkleParams"] = std::move(Cloth);
	}

	json TriArray = json::array();
	for (uint32 Tri : TriangleIndices)
	{
		TriArray.push_back(Tri);
	}
	Root["TriangleIndices"] = std::move(TriArray);

	const std::filesystem::path NativePath = FPaths::FromUtf8(FilePath);
	std::error_code DirError;
	if (!NativePath.parent_path().empty())
	{
		std::filesystem::create_directories(NativePath.parent_path(), DirError);
	}

	std::ofstream File(NativePath, std::ios::trunc);
	if (!File.is_open())
	{
		return false;
	}
	File << Root.dump(2);
	File.flush();
	return !File.fail();
}

bool UStockingRegion::LoadFromFile(const FString& FilePath, FString& OutError)
{
	OutError.clear();
	if (FilePath.empty())
	{
		OutError = "StockingRegion: empty file path.";
		return false;
	}

	const std::filesystem::path NativePath = FPaths::FromUtf8(FilePath);
	std::ifstream File(NativePath);
	if (!File.is_open())
	{
		OutError = "StockingRegion: could not open file: " + FilePath;
		return false;
	}

	json Root = json::parse(File, nullptr, false);
	if (!Root.is_object())
	{
		OutError = "StockingRegion: invalid JSON format.";
		return false;
	}

	SourceMeshPath = Root.value("SourceMeshPath", FString{});
	SourceLODIndex = Root.value("SourceLODIndex", 0);
	SourceVertexCount = Root.value("SourceVertexCount", 0u);
	SourceTriangleCount = Root.value("SourceTriangleCount", 0u);
	SourceTopologyHash = Root.value("SourceTopologyHash", 0ull);
	SubdivisionIterations = Root.value("SubdivisionIterations", 0);
	SurfaceOffset = Root.value("SurfaceOffset", 0.0025f);
	BoundaryFeatherDistance = Root.value("BoundaryFeatherDistance", 0.045f);

	bHasSavedMaterialParams = false;
	if (Root.contains("MaterialParams") && Root["MaterialParams"].is_object())
	{
		const json& Mat = Root["MaterialParams"];
		FStockingMaterialParams LoadedMat{};
		if (Mat.contains("FabricColor") && Mat["FabricColor"].is_array() && Mat["FabricColor"].size() >= 3)
		{
			LoadedMat.FabricColor = FVector4(
				Mat["FabricColor"][0].get<float>(),
				Mat["FabricColor"][1].get<float>(),
				Mat["FabricColor"][2].get<float>(),
				Mat["FabricColor"].size() >= 4 ? Mat["FabricColor"][3].get<float>() : 1.0f);
		}
		if (Mat.contains("HemColor") && Mat["HemColor"].is_array() && Mat["HemColor"].size() >= 3)
		{
			LoadedMat.HemColor = FVector4(
				Mat["HemColor"][0].get<float>(),
				Mat["HemColor"][1].get<float>(),
				Mat["HemColor"][2].get<float>(),
				Mat["HemColor"].size() >= 4 ? Mat["HemColor"][3].get<float>() : 1.0f);
		}
		LoadedMat.BaseOpacity = Mat.value("BaseOpacity", LoadedMat.BaseOpacity);
		LoadedMat.FresnelOpacity = Mat.value("FresnelOpacity", LoadedMat.FresnelOpacity);
		LoadedMat.FresnelPower = Mat.value("FresnelPower", LoadedMat.FresnelPower);
		LoadedMat.StretchSensitivity = Mat.value("StretchSensitivity", LoadedMat.StretchSensitivity);
		LoadedMat.StretchAlphaDrop = Mat.value("StretchAlphaDrop", LoadedMat.StretchAlphaDrop);
		LoadedMat.MinOpacity = Mat.value("MinOpacity", LoadedMat.MinOpacity);
		LoadedMat.WeaveScale = Mat.value("WeaveScale", LoadedMat.WeaveScale);
		LoadedMat.WeaveStrength = Mat.value("WeaveStrength", LoadedMat.WeaveStrength);
		LoadedMat.HemFeatherPower = Mat.value("HemFeatherPower", LoadedMat.HemFeatherPower);
		LoadedMat.HemBandStrength = Mat.value("HemBandStrength", LoadedMat.HemBandStrength);
		LoadedMat.SheenStrength = Mat.value("SheenStrength", LoadedMat.SheenStrength);
		LoadedMat.SpecularIntensity = Mat.value("SpecularIntensity", LoadedMat.SpecularIntensity);
		LoadedMat.SpecularPower = Mat.value("SpecularPower", LoadedMat.SpecularPower);
		LoadedMat.WrinkleShadingStrength = Mat.value("WrinkleShadingStrength", LoadedMat.WrinkleShadingStrength);
		SavedMaterialParams = LoadedMat;
		bHasSavedMaterialParams = true;
	}

	bHasSavedPullParams = false;
	if (Root.contains("PullParams") && Root["PullParams"].is_object())
	{
		const json& Pull = Root["PullParams"];
		SavedPullRadius = Pull.value("PullRadius", SavedPullRadius);
		SavedMaxPullDistance = Pull.value("MaxPullDistance", SavedMaxPullDistance);
		SavedPullFalloffExponent = Pull.value("PullFalloffExponent", SavedPullFalloffExponent);
		SavedWrinkleAmplitude = Pull.value("WrinkleAmplitude", SavedWrinkleAmplitude);
		SavedWrinkleFrequency = Pull.value("WrinkleFrequency", SavedWrinkleFrequency);
		SavedSpringStiffness = Pull.value("SpringStiffness", SavedSpringStiffness);
		SavedSpringDamping = Pull.value("SpringDamping", SavedSpringDamping);
		SavedBoundsPadding = Pull.value("BoundsPadding", SavedBoundsPadding);
		bHasSavedPullParams = true;
	}

	bHasSavedLegFilter = false;
	if (Root.contains("LegFilter") && Root["LegFilter"].is_object())
	{
		const json& Filter = Root["LegFilter"];
		SavedLegRadiusRatio = Filter.value("LegRadiusRatio", SavedLegRadiusRatio);
		SavedLegHeightRatio = Filter.value("LegHeightRatio", SavedLegHeightRatio);
		bHasSavedLegFilter = true;
	}

	bHasSavedClothWrinkleParams = false;
	if (Root.contains("ClothWrinkleParams") && Root["ClothWrinkleParams"].is_object())
	{
		const json& Cloth = Root["ClothWrinkleParams"];
		SavedUseNvClothSimulation = Cloth.value("UseNvClothSimulation", SavedUseNvClothSimulation);
		SavedClothJointWrinkleSlack = Cloth.value("JointWrinkleSlack", SavedClothJointWrinkleSlack);
		SavedClothBackstopRadius = Cloth.value("BackstopRadius", SavedClothBackstopRadius);
		SavedClothStretchStiffness = Cloth.value("StretchStiffness", SavedClothStretchStiffness);
		SavedClothBendStiffness = Cloth.value("BendStiffness", SavedClothBendStiffness);
		SavedClothShearStiffness = Cloth.value("ShearStiffness", SavedClothShearStiffness);
		SavedClothCompressionLimit = Cloth.value("CompressionLimit", SavedClothCompressionLimit);
		SavedClothStretchLimit = Cloth.value("StretchLimit", SavedClothStretchLimit);
		SavedClothMotionStiffness = Cloth.value("MotionStiffness", SavedClothMotionStiffness);
		SavedClothDamping = Cloth.value("Damping", SavedClothDamping);
		SavedClothSelfCollisionDistance = Cloth.value("SelfCollisionDistance", SavedClothSelfCollisionDistance);
		SavedClothSelfCollisionStiffness = Cloth.value("SelfCollisionStiffness", SavedClothSelfCollisionStiffness);
		bHasSavedClothWrinkleParams = true;
	}

	TArray<uint32> LoadedTriangles;
	if (Root.contains("TriangleIndices") && Root["TriangleIndices"].is_array())
	{
		LoadedTriangles.Reserve(static_cast<int32>(Root["TriangleIndices"].size()));
		for (const json& Entry : Root["TriangleIndices"])
		{
			if (Entry.is_number_unsigned() || Entry.is_number_integer())
			{
				LoadedTriangles.Add(Entry.get<uint32>());
			}
		}
	}
	SetTriangles(LoadedTriangles);
	RegionFilePath = FilePath;
	return true;
}

void UStockingRegion::Serialize(FStructuredArchive::FRecord Record)
{
	Super::Serialize(Record);

	// 리플렉션 외 메타데이터와 프리셋을 PIE 복제 시 함께 유지한다
	Record << SA_VALUE("SourceTopologyHash", SourceTopologyHash)
		<< SA_VALUE("RegionFilePath", RegionFilePath)
		<< SA_VALUE("bHasSavedMaterialParams", bHasSavedMaterialParams)
		<< SA_VALUE("bHasSavedPullParams", bHasSavedPullParams)
		<< SA_VALUE("bHasSavedLegFilter", bHasSavedLegFilter)
		<< SA_VALUE("bHasSavedClothWrinkleParams", bHasSavedClothWrinkleParams);

	if (bHasSavedMaterialParams)
	{
		Record << SA_VALUE("SavedFabricColor", SavedMaterialParams.FabricColor)
			<< SA_VALUE("SavedHemColor", SavedMaterialParams.HemColor)
			<< SA_VALUE("SavedBaseOpacity", SavedMaterialParams.BaseOpacity)
			<< SA_VALUE("SavedFresnelOpacity", SavedMaterialParams.FresnelOpacity)
			<< SA_VALUE("SavedFresnelPower", SavedMaterialParams.FresnelPower)
			<< SA_VALUE("SavedStretchSensitivity", SavedMaterialParams.StretchSensitivity)
			<< SA_VALUE("SavedStretchAlphaDrop", SavedMaterialParams.StretchAlphaDrop)
			<< SA_VALUE("SavedMinOpacity", SavedMaterialParams.MinOpacity)
			<< SA_VALUE("SavedWeaveScale", SavedMaterialParams.WeaveScale)
			<< SA_VALUE("SavedWeaveStrength", SavedMaterialParams.WeaveStrength)
			<< SA_VALUE("SavedHemFeatherPower", SavedMaterialParams.HemFeatherPower)
			<< SA_VALUE("SavedHemBandStrength", SavedMaterialParams.HemBandStrength)
			<< SA_VALUE("SavedSheenStrength", SavedMaterialParams.SheenStrength)
			<< SA_VALUE("SavedSpecularIntensity", SavedMaterialParams.SpecularIntensity)
			<< SA_VALUE("SavedSpecularPower", SavedMaterialParams.SpecularPower)
			<< SA_VALUE("SavedWrinkleShadingStrength", SavedMaterialParams.WrinkleShadingStrength)
			<< SA_VALUE("SavedTearRimStrength", SavedMaterialParams.TearRimStrength)
			<< SA_VALUE("SavedTearSkinContactStrength", SavedMaterialParams.TearSkinContactStrength)
			<< SA_VALUE("SavedTearRungStrength", SavedMaterialParams.TearRungStrength);
	}

	if (bHasSavedPullParams)
	{
		Record << SA_VALUE("SavedPullRadius", SavedPullRadius)
			<< SA_VALUE("SavedMaxPullDistance", SavedMaxPullDistance)
			<< SA_VALUE("SavedPullFalloffExponent", SavedPullFalloffExponent)
			<< SA_VALUE("SavedWrinkleAmplitude", SavedWrinkleAmplitude)
			<< SA_VALUE("SavedWrinkleFrequency", SavedWrinkleFrequency)
			<< SA_VALUE("SavedSpringStiffness", SavedSpringStiffness)
			<< SA_VALUE("SavedSpringDamping", SavedSpringDamping)
			<< SA_VALUE("SavedBoundsPadding", SavedBoundsPadding);
	}

	if (bHasSavedLegFilter)
	{
		Record << SA_VALUE("SavedLegRadiusRatio", SavedLegRadiusRatio)
			<< SA_VALUE("SavedLegHeightRatio", SavedLegHeightRatio);
	}

	if (bHasSavedClothWrinkleParams)
	{
		Record << SA_VALUE("SavedUseNvClothSimulation", SavedUseNvClothSimulation)
			<< SA_VALUE("SavedClothJointWrinkleSlack", SavedClothJointWrinkleSlack)
			<< SA_VALUE("SavedClothBackstopRadius", SavedClothBackstopRadius)
			<< SA_VALUE("SavedClothStretchStiffness", SavedClothStretchStiffness)
			<< SA_VALUE("SavedClothBendStiffness", SavedClothBendStiffness)
			<< SA_VALUE("SavedClothShearStiffness", SavedClothShearStiffness)
			<< SA_VALUE("SavedClothCompressionLimit", SavedClothCompressionLimit)
			<< SA_VALUE("SavedClothStretchLimit", SavedClothStretchLimit)
			<< SA_VALUE("SavedClothMotionStiffness", SavedClothMotionStiffness)
			<< SA_VALUE("SavedClothDamping", SavedClothDamping)
			<< SA_VALUE("SavedClothSelfCollisionDistance", SavedClothSelfCollisionDistance)
			<< SA_VALUE("SavedClothSelfCollisionStiffness", SavedClothSelfCollisionStiffness);
	}
}

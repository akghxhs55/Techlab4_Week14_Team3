#include "EnginePCH.h"
#include "GPUOcclusion.h"

#include "Render/RenderDevice.h"
#include "Render/Renderer.h"
#include "Render/RenderCommand.h"
#include "Render/RenderResourceManager.h"
#include "Render/Mesh.h"
#include "Render/Material.h"
#include "Render/Shader.h"
#include "Engine/PrimitiveSceneProxy.h"
#include "Asset/LOD/StaticMeshLODSelector.h"
#include "Core/Async/TaskPool.h"
#include "Core/Stats/LightweightStats.h"

#include <d3dcompiler.h>
#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cstring>

DECLARE_CYCLE_STAT("GPU Occlusion (Total)", STAT_GPUOcclusion);
DECLARE_CYCLE_STAT("  Occ Prepare+Occluders", STAT_GPUOcclusionPrepare);
DECLARE_CYCLE_STAT("  Occ Draw", STAT_GPUOcclusionDraw);
DECLARE_CYCLE_STAT("  Occ Wait GPU", STAT_GPUOcclusionWait);

namespace
{
	// 내장 가림막 깊이 패스와 컴퓨트 셰이더 소스
	constexpr char OcclusionShaderSource[] = R"(
#pragma pack_matrix(row_major)

cbuffer OccluderViewCB : register(b0)
{
	matrix OccluderViewProjection;
};

cbuffer OccluderObjectCB : register(b2)
{
	matrix OccluderWorld;
};

float4 VSOccluder(float3 Position : POSITION) : SV_POSITION
{
	return mul(mul(float4(Position, 1.0f), OccluderWorld), OccluderViewProjection);
}

cbuffer HiZParams : register(b0)
{
	uint2 SrcSize;
	uint2 DstSize;
};
Texture2D<float> HiZSrc : register(t0);
RWTexture2D<float> HiZDst : register(u0);

[numthreads(8, 8, 1)]
void BuildHiZ(uint3 Id : SV_DispatchThreadID)
{
	if (Id.x >= DstSize.x || Id.y >= DstSize.y)
		return;

	const uint2 First = Id.xy * 2;
	uint2 Last = min(First + 1, SrcSize - 1);
	if (Id.x == DstSize.x - 1) Last.x = SrcSize.x - 1;
	if (Id.y == DstSize.y - 1) Last.y = SrcSize.y - 1;

	float MaxDepth = 0.0f;
	for (uint Y = First.y; Y <= Last.y; ++Y)
		for (uint X = First.x; X <= Last.x; ++X)
			MaxDepth = max(MaxDepth, HiZSrc.Load(int3(X, Y, 0)));
	HiZDst[Id.xy] = MaxDepth;
}

cbuffer CullParams : register(b1)
{
	matrix ViewProjection;
	float2 ScreenSize;
	uint NumItems;
	uint Padding;
};

struct FCullItem
{
	float3 Center;
	float Padding0;
	float3 Extent;
	float Padding1;
};
StructuredBuffer<FCullItem> Items : register(t1);
Texture2D<float> HiZ : register(t2);
RWBuffer<uint> Result : register(u1);

bool IsOccluded(FCullItem Item)
{
	if (Item.Extent.x < 0.0f)
		return false;

	float2 NdcMin = 1e30f, NdcMax = -1e30f;
	float MinZ = 1.0f;
	[unroll]
	for (uint c = 0; c < 8; ++c)
	{
		const float3 Sign = float3((c & 1) ? 1.0f : -1.0f, (c & 2) ? 1.0f : -1.0f, (c & 4) ? 1.0f : -1.0f);
		const float4 Clip = mul(float4(Item.Center + Item.Extent * Sign, 1.0f), ViewProjection);
		if (Clip.w <= 1e-5f || Clip.z < 0.0f)
			return false;
		const float3 Ndc = Clip.xyz / Clip.w;
		NdcMin = min(NdcMin, Ndc.xy);
		NdcMax = max(NdcMax, Ndc.xy);
		MinZ = min(MinZ, Ndc.z);
	}

	float2 UvMin = float2(NdcMin.x * 0.5f + 0.5f, 0.5f - NdcMax.y * 0.5f);
	float2 UvMax = float2(NdcMax.x * 0.5f + 0.5f, 0.5f - NdcMin.y * 0.5f);
	if (UvMax.x <= 0.0f || UvMax.y <= 0.0f || UvMin.x >= 1.0f || UvMin.y >= 1.0f)
		return false;
	UvMin = saturate(UvMin);
	UvMax = saturate(UvMax);

	const float2 TexelMin = UvMin * ScreenSize * 0.5f;
	const float2 TexelMax = UvMax * ScreenSize * 0.5f;
	const float Size = max(TexelMax.x - TexelMin.x, TexelMax.y - TexelMin.y);

	uint Width, Height, Levels;
	HiZ.GetDimensions(0, Width, Height, Levels);
	uint Level = Size <= 1.0f ? 0 : (uint)ceil(log2(Size));
	Level = min(Level, Levels - 1);
	HiZ.GetDimensions(Level, Width, Height, Levels);

	const int2 MaxTexel = int2(Width - 1, Height - 1);
	const int2 T0 = min(int2(TexelMin) >> Level, MaxTexel);
	const int2 T1 = min(int2(TexelMax) >> Level, MaxTexel);

	float MaxDepth = 0.0f;
	for (int Y = T0.y; Y <= T1.y; ++Y)
		for (int X = T0.x; X <= T1.x; ++X)
			MaxDepth = max(MaxDepth, HiZ.Load(int3(X, Y, Level)));

	return MinZ > MaxDepth;
}

[numthreads(64, 1, 1)]
void Cull(uint3 Id : SV_DispatchThreadID)
{
	if (Id.x >= NumItems)
		return;
	Result[Id.x] = IsOccluded(Items[Id.x]) ? 1 : 0;
}
)";

	ComPtr<ID3D11ComputeShader> CompileComputeShader(ID3D11Device* Device, const char* EntryPoint)
	{
		const UINT Flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
		ComPtr<ID3DBlob> Code, Errors;
		const HRESULT Hr = D3DCompile(OcclusionShaderSource, sizeof(OcclusionShaderSource) - 1, "GPUOcclusion",
			nullptr, nullptr, EntryPoint, "cs_5_0", Flags, 0, Code.GetAddressOf(), Errors.GetAddressOf());
		if (FAILED(Hr))
		{
			if (Errors)
			{
				OutputDebugStringA(static_cast<const char*>(Errors->GetBufferPointer()));
			}
			KRS_LOG(Error, "[GPUOcclusion] compute shader {} compile failed", EntryPoint);
			return nullptr;
		}

		ComPtr<ID3D11ComputeShader> Shader;
		if (FAILED(Device->CreateComputeShader(Code->GetBufferPointer(), Code->GetBufferSize(), nullptr, Shader.GetAddressOf())))
		{
			return nullptr;
		}
		return Shader;
	}

	bool CompileOccluderVertexShader(ID3D11Device* Device, ComPtr<ID3D11VertexShader>& OutVS, ComPtr<ID3D11InputLayout>& OutLayout)
	{
		const UINT Flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
		ComPtr<ID3DBlob> Code, Errors;
		const HRESULT Hr = D3DCompile(OcclusionShaderSource, sizeof(OcclusionShaderSource) - 1, "GPUOcclusion",
			nullptr, nullptr, "VSOccluder", "vs_5_0", Flags, 0, Code.GetAddressOf(), Errors.GetAddressOf());
		if (FAILED(Hr))
		{
			if (Errors)
			{
				OutputDebugStringA(static_cast<const char*>(Errors->GetBufferPointer()));
			}
			KRS_LOG(Error, "[GPUOcclusion] vertex shader VSOccluder compile failed");
			return false;
		}

		if (FAILED(Device->CreateVertexShader(Code->GetBufferPointer(), Code->GetBufferSize(), nullptr, OutVS.GetAddressOf())))
		{
			return false;
		}

		const D3D11_INPUT_ELEMENT_DESC LayoutDesc[] =
		{
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 }
		};
		return SUCCEEDED(Device->CreateInputLayout(LayoutDesc, 1, Code->GetBufferPointer(), Code->GetBufferSize(), OutLayout.GetAddressOf()));
	}

	ComPtr<ID3D11Buffer> CreateDefaultConstantBuffer(ID3D11Device* Device, uint32 Size)
	{
		D3D11_BUFFER_DESC Desc{};
		Desc.ByteWidth = (Size + 15) & ~15u;
		Desc.Usage = D3D11_USAGE_DEFAULT;
		Desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		ComPtr<ID3D11Buffer> Buffer;
		Device->CreateBuffer(&Desc, nullptr, Buffer.GetAddressOf());
		return Buffer;
	}

	struct FHiZParams
	{
		uint32 SrcSize[2];
		uint32 DstSize[2];
	};

	struct FCullParams
	{
		FMatrix ViewProjection;
		float ScreenSize[2];
		uint32 NumItems;
		uint32 Padding;
	};

	struct FOccluderDraw
	{
		uint32 Slot;
		uint32 StartIndex;
		uint32 IndexCount;
	};

	struct FOccluderBucket
	{
		UStaticMesh* Mesh;
		uint32 LOD;
		std::vector<FOccluderDraw> Draws;
	};
	std::vector<FOccluderBucket> OccluderBuckets;
	constexpr uint32 OccluderSlotBytes = 256;
	constexpr uint32 ScoreBucketCount = 256;
	constexpr int32 ScoreBucketOffset = 160;

	uint8 ToScoreBucket(float Score)
	{
		const int32 Bucket = static_cast<int32>(std::floor(std::log2(Score) * 4.0f)) + ScoreBucketOffset;
		return static_cast<uint8>(std::clamp(Bucket, 1, static_cast<int32>(ScoreBucketCount) - 1));
	}
}

bool FGPUOcclusion::Init()
{
	ID3D11Device* Device = FRenderDevice::Get()->GetDevice();
	if (!Device)
	{
		return false;
	}

	BuildHiZCS = CompileComputeShader(Device, "BuildHiZ");
	CullCS = CompileComputeShader(Device, "Cull");
	const bool bHasOccluderVS = CompileOccluderVertexShader(Device, OccluderVS, OccluderInputLayout);
	HiZParamsCB = CreateDefaultConstantBuffer(Device, sizeof(FHiZParams));
	CullParamsCB = CreateDefaultConstantBuffer(Device, sizeof(FCullParams));
	ViewCB = CreateDefaultConstantBuffer(Device, sizeof(FMatrix));

	const bool bReady = BuildHiZCS && CullCS && bHasOccluderVS && HiZParamsCB && CullParamsCB && ViewCB &&
		RenderCommand::SupportsConstantBufferOffsets();
	if (!bReady)
	{
		KRS_LOG(Error, "[GPUOcclusion] init failed. GPU occlusion culling is unavailable.");
	}
	return bReady;
}

void FGPUOcclusion::SwapActiveTargets(FTargetSet& Other)
{
	std::swap(TargetWidth, Other.TargetWidth);
	std::swap(TargetHeight, Other.TargetHeight);
	OccluderDepth.Swap(Other.OccluderDepth);
	OccluderDSV.Swap(Other.OccluderDSV);
	OccluderDepthSRV.Swap(Other.OccluderDepthSRV);
	HiZ.Swap(Other.HiZ);
	HiZSRV.Swap(Other.HiZSRV);
	HiZMipSRVs.swap(Other.HiZMipSRVs);
	HiZMipUAVs.swap(Other.HiZMipUAVs);
	MipWidths.swap(Other.MipWidths);
	MipHeights.swap(Other.MipHeights);
	std::swap(ActiveTargetLastUse, Other.LastUse);
}

bool FGPUOcclusion::EnsureTargets(uint32 Width, uint32 Height)
{
	++TargetUseCounter;
	if (OccluderDSV && TargetWidth == Width && TargetHeight == Height)
	{
		ActiveTargetLastUse = TargetUseCounter;
		return true;
	}

	for (FTargetSet& Spare : SpareTargets)
	{
		if (Spare.OccluderDSV && Spare.TargetWidth == Width && Spare.TargetHeight == Height)
		{
			SwapActiveTargets(Spare);
			ActiveTargetLastUse = TargetUseCounter;
			return true;
		}
	}

	if (OccluderDSV)
	{
		FTargetSet* Slot = nullptr;
		if (SpareTargets.size() < MaxSpareTargets)
		{
			Slot = &SpareTargets.emplace_back();
		}
		else
		{
			Slot = &*std::min_element(SpareTargets.begin(), SpareTargets.end(),
				[](const FTargetSet& A, const FTargetSet& B) { return A.LastUse < B.LastUse; });
		}
		*Slot = FTargetSet{};
		SwapActiveTargets(*Slot);
	}
	ActiveTargetLastUse = TargetUseCounter;

	ID3D11Device* Device = FRenderDevice::Get()->GetDevice();
	TargetWidth = TargetHeight = 0;
	OccluderDepth.Reset();
	OccluderDSV.Reset();
	OccluderDepthSRV.Reset();
	HiZ.Reset();
	HiZSRV.Reset();
	HiZMipSRVs.clear();
	HiZMipUAVs.clear();
	MipWidths.clear();
	MipHeights.clear();

	D3D11_TEXTURE2D_DESC DepthDesc{};
	DepthDesc.Width = Width;
	DepthDesc.Height = Height;
	DepthDesc.MipLevels = 1;
	DepthDesc.ArraySize = 1;
	DepthDesc.Format = DXGI_FORMAT_R32_TYPELESS;
	DepthDesc.SampleDesc.Count = 1;
	DepthDesc.Usage = D3D11_USAGE_DEFAULT;
	DepthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
	if (FAILED(Device->CreateTexture2D(&DepthDesc, nullptr, OccluderDepth.GetAddressOf())))
	{
		return false;
	}

	D3D11_DEPTH_STENCIL_VIEW_DESC DsvDesc{};
	DsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
	DsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
	D3D11_SHADER_RESOURCE_VIEW_DESC DepthSrvDesc{};
	DepthSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
	DepthSrvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	DepthSrvDesc.Texture2D.MipLevels = 1;
	if (FAILED(Device->CreateDepthStencilView(OccluderDepth.Get(), &DsvDesc, OccluderDSV.GetAddressOf())) ||
		FAILED(Device->CreateShaderResourceView(OccluderDepth.Get(), &DepthSrvDesc, OccluderDepthSRV.GetAddressOf())))
	{
		return false;
	}

	const uint32 Width0 = std::max(1u, (Width + 1) / 2);
	const uint32 Height0 = std::max(1u, (Height + 1) / 2);
	uint32 MipCount = 1;
	for (uint32 Size = std::max(Width0, Height0); Size > 1; Size >>= 1)
	{
		++MipCount;
	}

	D3D11_TEXTURE2D_DESC HiZDesc{};
	HiZDesc.Width = Width0;
	HiZDesc.Height = Height0;
	HiZDesc.MipLevels = MipCount;
	HiZDesc.ArraySize = 1;
	HiZDesc.Format = DXGI_FORMAT_R32_FLOAT;
	HiZDesc.SampleDesc.Count = 1;
	HiZDesc.Usage = D3D11_USAGE_DEFAULT;
	HiZDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	if (FAILED(Device->CreateTexture2D(&HiZDesc, nullptr, HiZ.GetAddressOf())) ||
		FAILED(Device->CreateShaderResourceView(HiZ.Get(), nullptr, HiZSRV.GetAddressOf())))
	{
		return false;
	}

	HiZMipSRVs.resize(MipCount);
	HiZMipUAVs.resize(MipCount);
	for (uint32 Level = 0; Level < MipCount; ++Level)
	{
		MipWidths.push_back(std::max(1u, Width0 >> Level));
		MipHeights.push_back(std::max(1u, Height0 >> Level));

		D3D11_SHADER_RESOURCE_VIEW_DESC SrvDesc{};
		SrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
		SrvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		SrvDesc.Texture2D.MostDetailedMip = Level;
		SrvDesc.Texture2D.MipLevels = 1;
		D3D11_UNORDERED_ACCESS_VIEW_DESC UavDesc{};
		UavDesc.Format = DXGI_FORMAT_R32_FLOAT;
		UavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
		UavDesc.Texture2D.MipSlice = Level;
		if (FAILED(Device->CreateShaderResourceView(HiZ.Get(), &SrvDesc, HiZMipSRVs[Level].GetAddressOf())) ||
			FAILED(Device->CreateUnorderedAccessView(HiZ.Get(), &UavDesc, HiZMipUAVs[Level].GetAddressOf())))
		{
			return false;
		}
	}

	TargetWidth = Width;
	TargetHeight = Height;
	return true;
}

bool FGPUOcclusion::EnsureItemCapacity(uint32 Count)
{
	if (Count <= ItemCapacity && ItemBuffer && ResultBuffer)
	{
		return true;
	}

	uint32 NewCapacity = std::max(ItemCapacity * 2, 4096u);
	while (NewCapacity < Count)
	{
		NewCapacity *= 2;
	}

	ID3D11Device* Device = FRenderDevice::Get()->GetDevice();
	ItemBuffer.Reset();
	ItemSRV.Reset();
	ResultBuffer.Reset();
	ResultUAV.Reset();
	ResultStaging.Reset();
	ItemCapacity = 0;

	D3D11_BUFFER_DESC ItemDesc{};
	ItemDesc.ByteWidth = NewCapacity * sizeof(FCullItem);
	ItemDesc.Usage = D3D11_USAGE_DYNAMIC;
	ItemDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	ItemDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	ItemDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
	ItemDesc.StructureByteStride = sizeof(FCullItem);
	D3D11_SHADER_RESOURCE_VIEW_DESC ItemSrvDesc{};
	ItemSrvDesc.Format = DXGI_FORMAT_UNKNOWN;
	ItemSrvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
	ItemSrvDesc.Buffer.NumElements = NewCapacity;
	if (FAILED(Device->CreateBuffer(&ItemDesc, nullptr, ItemBuffer.GetAddressOf())) ||
		FAILED(Device->CreateShaderResourceView(ItemBuffer.Get(), &ItemSrvDesc, ItemSRV.GetAddressOf())))
	{
		return false;
	}

	D3D11_BUFFER_DESC ResultDesc{};
	ResultDesc.ByteWidth = NewCapacity * sizeof(uint32);
	ResultDesc.Usage = D3D11_USAGE_DEFAULT;
	ResultDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
	D3D11_UNORDERED_ACCESS_VIEW_DESC ResultUavDesc{};
	ResultUavDesc.Format = DXGI_FORMAT_R32_UINT;
	ResultUavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
	ResultUavDesc.Buffer.NumElements = NewCapacity;
	if (FAILED(Device->CreateBuffer(&ResultDesc, nullptr, ResultBuffer.GetAddressOf())) ||
		FAILED(Device->CreateUnorderedAccessView(ResultBuffer.Get(), &ResultUavDesc, ResultUAV.GetAddressOf())))
	{
		return false;
	}

	D3D11_BUFFER_DESC StagingDesc{};
	StagingDesc.ByteWidth = NewCapacity * sizeof(uint32);
	StagingDesc.Usage = D3D11_USAGE_STAGING;
	StagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	if (FAILED(Device->CreateBuffer(&StagingDesc, nullptr, ResultStaging.GetAddressOf())))
	{
		return false;
	}

	ItemCapacity = NewCapacity;
	return true;
}

bool FGPUOcclusion::EnsureOccluderCapacity(uint32 Count)
{
	if (Count <= OccluderCapacity && OccluderCB)
	{
		return true;
	}

	uint32 NewCapacity = std::max(OccluderCapacity * 2, 1024u);
	while (NewCapacity < Count)
	{
		NewCapacity *= 2;
	}
	OccluderCB = RenderCommand::CreateConstantBuffer(NewCapacity * OccluderSlotBytes);
	OccluderCapacity = (OccluderCB && OccluderCB->GetBuffer()) ? NewCapacity : 0;
	return OccluderCapacity != 0;
}

bool FGPUOcclusion::FillItems(const FPrimitiveSceneProxy* const* Proxies, uint32 Count, const FViewContext& View)
{
	ScoreBuckets.resize(Count);
	OccluderLODs.resize(Count);

	const float Aspect = static_cast<float>(std::min(View.Width, View.Height)) / static_cast<float>(std::max(View.Width, View.Height));
	CoverageScale = 3.14159265f / 12.0f * View.ProjectionScaleSquared * Aspect;

	FTaskPool& Pool = FTaskPool::Get();
	const uint32 ChunkCount = std::clamp(Count / 1024u, 1u, Pool.GetNumThreads() * 4);
	HistogramCounts.assign(size_t(ChunkCount) * ScoreBucketCount, 0u);
	HistogramScores.assign(size_t(ChunkCount) * ScoreBucketCount, 0.0f);
	HistogramChunks = ChunkCount;

	D3D11_MAPPED_SUBRESOURCE Mapped{};
	if (FAILED(FRenderDevice::Get()->GetContext()->Map(ItemBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &Mapped)))
	{
		std::fill(ScoreBuckets.begin(), ScoreBuckets.end(), uint8(0));
		return false;
	}
	FCullItem* Dest = static_cast<FCullItem*>(Mapped.pData);

	Pool.ParallelFor(Count, ChunkCount, [&](uint32 Begin, uint32 End, uint32 ChunkIndex)
		{
			uint32* Counts = HistogramCounts.data() + size_t(ChunkIndex) * ScoreBucketCount;
			float* ScoreSums = HistogramScores.data() + size_t(ChunkIndex) * ScoreBucketCount;

			for (uint32 i = Begin; i < End; ++i)
			{
				const FPrimitiveSceneProxy* Proxy = Proxies[i];
				const FAABB& Bounds = Proxy->GetBounds();

				FCullItem Item{};
				Item.Center = Bounds.Center;
				Item.Extent = Bounds.Extent;
				std::memcpy(Dest + i, &Item, sizeof(FCullItem));

				uint8 Bucket = 0;
				if (Proxy->IsVisible() && Proxy->GetMesh() && Proxy->HasStaticRelevance())
				{
					const FVector ToCenter = Bounds.Center - View.CameraPosition;
					const float Score = Bounds.Extent.Dot(Bounds.Extent) / std::max(ToCenter.Dot(ToCenter), 1e-4f);
					Bucket = ToScoreBucket(Score);
					++Counts[Bucket];
					ScoreSums[Bucket] += Score;
					OccluderLODs[i] = static_cast<uint8>(SelectLOD(*Proxy, View));
				}
				ScoreBuckets[i] = Bucket;
			}
		});

	FRenderDevice::Get()->GetContext()->Unmap(ItemBuffer.Get(), 0);
	return true;
}

void FGPUOcclusion::DrawOccluders(const FPrimitiveSceneProxy* const* Proxies, const FViewContext& View)
{
	ID3D11DeviceContext* Context = FRenderDevice::Get()->GetContext();
	Context->ClearDepthStencilView(OccluderDSV.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);

	{
		const uint32 Count = static_cast<uint32>(ScoreBuckets.size());
		uint32 Counts[ScoreBucketCount] = {};
		double ScoreSums[ScoreBucketCount] = {};
		for (uint32 c = 0; c < HistogramChunks; ++c)
		{
			for (uint32 b = 0; b < ScoreBucketCount; ++b)
			{
				Counts[b] += HistogramCounts[size_t(c) * ScoreBucketCount + b];
				ScoreSums[b] += HistogramScores[size_t(c) * ScoreBucketCount + b];
			}
		}

		const uint32 Budget = std::min(Settings.MaxOccluders, Count);
		uint32 Cut = ScoreBucketCount;
		uint32 Accumulated = 0;
		double Coverage = 0.0;
		for (int32 b = ScoreBucketCount - 1; b >= 1 && Budget > 0; --b)
		{
			if (Counts[b] == 0)
			{
				continue;
			}
			Cut = static_cast<uint32>(b);
			Accumulated += Counts[b];
			Coverage += ScoreSums[b] * CoverageScale;
			if (Accumulated >= Budget || Coverage >= Settings.OccluderCoverage)
			{
				break;
			}
		}
		Stats.Coverage = static_cast<float>(Coverage);

		for (FOccluderBucket& Bucket : OccluderBuckets)
		{
			Bucket.Draws.clear();
		}
		Stats.Occluders = 0;
		Stats.OccluderTriangles = 0;

		if (Budget == 0 || Cut >= ScoreBucketCount || !EnsureOccluderCapacity(Budget))
		{
			return;
		}

		uint8* Slots = static_cast<uint8*>(RenderCommand::MapWriteDiscard(OccluderCB.get()));
		if (!Slots)
		{
			return;
		}

		uint32 Selected = 0;
		for (uint32 Pass = 0; Pass < 2; ++Pass)
		{
			for (uint32 i = 0; i < Count && Selected < Budget; ++i)
			{
				const uint32 ScoreBucket = ScoreBuckets[i];
				if (Pass == 0 ? ScoreBucket <= Cut : ScoreBucket != Cut)
				{
					continue;
				}

				const FPrimitiveSceneProxy* Proxy = Proxies[i];
				const uint32 LOD = OccluderLODs[i];
				const FCachedMeshLOD& CachedLOD = Proxy->GetLOD(LOD);

				bool bAnyOpaque = false;
				for (uint32 s = 0; s < CachedLOD.NumSections; ++s)
				{
					const FCachedMeshSection& Section = Proxy->GetSection(CachedLOD.FirstSection + s);
					if (!Section.Material || Section.Material->GetBlendState() != EBlendState::Opaque)
					{
						continue;
					}

					FOccluderBucket* Target = nullptr;
					for (FOccluderBucket& Bucket : OccluderBuckets)
					{
						if (Bucket.Mesh == Proxy->GetMesh() && Bucket.LOD == LOD)
						{
							Target = &Bucket;
							break;
						}
					}
					if (!Target)
					{
						Target = &OccluderBuckets.emplace_back(FOccluderBucket{ Proxy->GetMesh(), LOD, {} });
					}
					Target->Draws.push_back({ Selected, Section.StartIndex, Section.IndexCount });
					Stats.OccluderTriangles += Section.IndexCount / 3;
					bAnyOpaque = true;
				}

				if (bAnyOpaque)
				{
					std::memcpy(Slots + size_t(Selected) * OccluderSlotBytes, &Proxy->GetLocalToWorld(), sizeof(FMatrix));
					++Selected;
				}
			}
		}
		RenderCommand::Unmap(OccluderCB.get());
		Stats.Occluders = Selected;
	}

	SCOPE_CYCLE_COUNTER(STAT_GPUOcclusionDraw);

	Context->UpdateSubresource(ViewCB.Get(), 0, nullptr, &View.ViewProjection, 0, 0);

	Context->OMSetRenderTargets(0, nullptr, OccluderDSV.Get());
	const D3D11_VIEWPORT Viewport{ 0.0f, 0.0f, static_cast<float>(TargetWidth), static_cast<float>(TargetHeight), 0.0f, 1.0f };
	Context->RSSetViewports(1, &Viewport);

	RenderCommand::SetDepthStencilState(EDepthStencilState::Default);
	RenderCommand::SetRasterizerState(ERasterizerState::SolidBack);
	RenderCommand::SetPrimitiveTopology(EPrimitiveTopology::TriangleList);
	Context->VSSetConstantBuffers(0, 1, ViewCB.GetAddressOf());
	Context->IASetInputLayout(OccluderInputLayout.Get());
	Context->VSSetShader(OccluderVS.Get(), nullptr, 0);
	Context->PSSetShader(nullptr, nullptr, 0);

	constexpr uint32 SlotConstants = OccluderSlotBytes / 16;
	for (const FOccluderBucket& Bucket : OccluderBuckets)
	{
		if (Bucket.Draws.empty() || !Bucket.Mesh)
		{
			continue;
		}
		FVertexBuffer* VB = Bucket.Mesh->GetLODVertexBuffer(Bucket.LOD);
		FIndexBuffer* IB = Bucket.Mesh->GetLODIndexBuffer(Bucket.LOD);
		if (!VB || !IB)
		{
			continue;
		}
		RenderCommand::BindVertexBuffer(VB);
		RenderCommand::BindIndexBuffer(IB);

		for (const FOccluderDraw& Draw : Bucket.Draws)
		{
			RenderCommand::BindConstantBufferRange(2, OccluderCB.get(), Draw.Slot * SlotConstants, SlotConstants, EShaderBindFlagBits::Vertex);
			RenderCommand::DrawIndexed(Draw.IndexCount, Draw.StartIndex);
		}
	}

	Context->VSSetShader(nullptr, nullptr, 0);
	Context->OMSetRenderTargets(0, nullptr, nullptr);
}

void FGPUOcclusion::BuildHiZ()
{
	ID3D11DeviceContext* Context = FRenderDevice::Get()->GetContext();
	Context->CSSetShader(BuildHiZCS.Get(), nullptr, 0);
	Context->CSSetConstantBuffers(0, 1, HiZParamsCB.GetAddressOf());

	ID3D11ShaderResourceView* NullSRV = nullptr;
	ID3D11UnorderedAccessView* NullUAV = nullptr;
	for (uint32 Level = 0; Level < static_cast<uint32>(HiZMipUAVs.size()); ++Level)
	{
		FHiZParams Params;
		Params.SrcSize[0] = Level == 0 ? TargetWidth : MipWidths[Level - 1];
		Params.SrcSize[1] = Level == 0 ? TargetHeight : MipHeights[Level - 1];
		Params.DstSize[0] = MipWidths[Level];
		Params.DstSize[1] = MipHeights[Level];
		Context->UpdateSubresource(HiZParamsCB.Get(), 0, nullptr, &Params, 0, 0);

		ID3D11ShaderResourceView* Src = Level == 0 ? OccluderDepthSRV.Get() : HiZMipSRVs[Level - 1].Get();
		Context->CSSetShaderResources(0, 1, &Src);
		Context->CSSetUnorderedAccessViews(0, 1, HiZMipUAVs[Level].GetAddressOf(), nullptr);
		Context->Dispatch((MipWidths[Level] + 7) / 8, (MipHeights[Level] + 7) / 8, 1);

		Context->CSSetUnorderedAccessViews(0, 1, &NullUAV, nullptr);
		Context->CSSetShaderResources(0, 1, &NullSRV);
	}
}

void FGPUOcclusion::Cull(const FMatrix& ViewProjection, uint32 Count)
{
	ID3D11DeviceContext* Context = FRenderDevice::Get()->GetContext();

	FCullParams Params;
	Params.ViewProjection = ViewProjection;
	Params.ScreenSize[0] = static_cast<float>(TargetWidth);
	Params.ScreenSize[1] = static_cast<float>(TargetHeight);
	Params.NumItems = Count;
	Params.Padding = 0;
	Context->UpdateSubresource(CullParamsCB.Get(), 0, nullptr, &Params, 0, 0);

	Context->CSSetShader(CullCS.Get(), nullptr, 0);
	Context->CSSetConstantBuffers(1, 1, CullParamsCB.GetAddressOf());
	ID3D11ShaderResourceView* SRVs[2] = { ItemSRV.Get(), HiZSRV.Get() };
	Context->CSSetShaderResources(1, 2, SRVs);
	Context->CSSetUnorderedAccessViews(1, 1, ResultUAV.GetAddressOf(), nullptr);

	Context->Dispatch((Count + 63) / 64, 1, 1);

	ID3D11ShaderResourceView* NullSRVs[3] = {};
	ID3D11UnorderedAccessView* NullUAVs[2] = {};
	Context->CSSetShaderResources(0, 3, NullSRVs);
	Context->CSSetUnorderedAccessViews(0, 2, NullUAVs, nullptr);
	Context->CSSetShader(nullptr, nullptr, 0);

	Context->CopyResource(ResultStaging.Get(), ResultBuffer.Get());
}

bool FGPUOcclusion::ReadBack(uint32 Count)
{
	SCOPE_CYCLE_COUNTER(STAT_GPUOcclusionWait);
	const std::chrono::high_resolution_clock::time_point Start = std::chrono::high_resolution_clock::now();

	ID3D11DeviceContext* Context = FRenderDevice::Get()->GetContext();
	D3D11_MAPPED_SUBRESOURCE Mapped{};
	if (FAILED(Context->Map(ResultStaging.Get(), 0, D3D11_MAP_READ, 0, &Mapped)))
	{
		return false;
	}

	const uint32* Result = static_cast<const uint32*>(Mapped.pData);
	Occluded.resize(Count);
	uint32 OccludedCount = 0;
	for (uint32 i = 0; i < Count; ++i)
	{
		Occluded[i] = static_cast<uint8>(Result[i]);
		OccludedCount += Result[i];
	}
	Context->Unmap(ResultStaging.Get(), 0);

	Stats.Occluded = OccludedCount;
	Stats.WaitMs = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - Start).count();
	return true;
}

bool FGPUOcclusion::Run(const FPrimitiveSceneProxy* const* Proxies, uint32 Count, const FViewContext& View)
{
	SCOPE_CYCLE_COUNTER(STAT_GPUOcclusion);

	Stats.Tested = Count;
	Stats.Occluded = 0;
	if (!Settings.bEnabled || Count == 0 || View.Width == 0 || View.Height == 0 || !CullCS || !BuildHiZCS || !OccluderVS)
	{
		return false;
	}
	if (!EnsureTargets(View.Width, View.Height) || !EnsureItemCapacity(Count))
	{
		return false;
	}

	ID3D11DeviceContext* Context = FRenderDevice::Get()->GetContext();

	ComPtr<ID3D11RenderTargetView> SavedRTV;
	ComPtr<ID3D11DepthStencilView> SavedDSV;
	Context->OMGetRenderTargets(1, SavedRTV.GetAddressOf(), SavedDSV.GetAddressOf());
	UINT ViewportCount = 1;
	D3D11_VIEWPORT SavedViewport{};
	Context->RSGetViewports(&ViewportCount, &SavedViewport);

	bool bFilled = false;
	{
		SCOPE_CYCLE_COUNTER(STAT_GPUOcclusionPrepare);
		bFilled = FillItems(Proxies, Count, View);
		if (bFilled)
		{
			DrawOccluders(Proxies, View);
			BuildHiZ();
			Cull(View.ViewProjection, Count);
		}
	}

	Context->OMSetRenderTargets(1, SavedRTV.GetAddressOf(), SavedDSV.Get());
	if (ViewportCount > 0)
	{
		Context->RSSetViewports(1, &SavedViewport);
	}

	return bFilled && ReadBack(Count);
}

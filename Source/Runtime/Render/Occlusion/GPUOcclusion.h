#pragma once

#include "Math/Matrix.h"
#include "Render/Buffer.h"

#include <d3d11.h>
#include <vector>

class FPrimitiveSceneProxy;
struct FViewContext;

struct FGPUOcclusionSettings
{
	bool bEnabled = true;
	// 비활성화 시 판정만 수행하고 컬링은 건너뜀
	bool bCull = true;
	float OccluderCoverage = 8.0f;
	uint32 MaxOccluders = 16384;
};

struct FGPUOcclusionStats
{
	uint32 Occluders = 0;
	uint64 OccluderTriangles = 0;
	float Coverage = 0.0f;
	uint32 Tested = 0;
	uint32 Occluded = 0;
	double WaitMs = 0.0;
};

// 매 프레임 현재 카메라 기준으로 Hi-Z 밉 체인 및 AABB 가려짐 판정 수행
class FGPUOcclusion
{
public:
	bool Init();

	// 프러스텀을 통과한 프록시 목록을 받아 가려짐 여부 계산
	bool Run(const FPrimitiveSceneProxy* const* Proxies, uint32 Count, const FViewContext& View);

	const std::vector<uint8>& GetOccluded() const { return Occluded; }

	FGPUOcclusionSettings& GetSettings() { return Settings; }
	const FGPUOcclusionStats& GetStats() const { return Stats; }

private:
	struct FCullItem
	{
		FVector Center;
		float Padding0;
		// 음수면 판정 제외하고 항상 가시 상태로 유지
		FVector Extent;
		float Padding1;
	};
	static_assert(sizeof(FCullItem) == 32);

	struct FTargetSet
	{
		uint32 TargetWidth = 0;
		uint32 TargetHeight = 0;
		ComPtr<ID3D11Texture2D> OccluderDepth;
		ComPtr<ID3D11DepthStencilView> OccluderDSV;
		ComPtr<ID3D11ShaderResourceView> OccluderDepthSRV;
		ComPtr<ID3D11Texture2D> HiZ;
		ComPtr<ID3D11ShaderResourceView> HiZSRV;
		std::vector<ComPtr<ID3D11ShaderResourceView>> HiZMipSRVs;
		std::vector<ComPtr<ID3D11UnorderedAccessView>> HiZMipUAVs;
		std::vector<uint32> MipWidths;
		std::vector<uint32> MipHeights;
		uint64 LastUse = 0;
	};
	static constexpr size_t MaxSpareTargets = 7;

	bool EnsureTargets(uint32 Width, uint32 Height);
	void SwapActiveTargets(FTargetSet& Other);
	bool EnsureItemCapacity(uint32 Count);
	bool EnsureOccluderCapacity(uint32 Count);
	bool FillItems(const FPrimitiveSceneProxy* const* Proxies, uint32 Count, const FViewContext& View);
	void DrawOccluders(const FPrimitiveSceneProxy* const* Proxies, const FViewContext& View);
	void BuildHiZ();
	void Cull(const FMatrix& ViewProjection, uint32 Count);
	bool ReadBack(uint32 Count);

	FGPUOcclusionSettings Settings;
	FGPUOcclusionStats Stats;
	std::vector<uint8> Occluded;

	ComPtr<ID3D11ComputeShader> BuildHiZCS;
	ComPtr<ID3D11ComputeShader> CullCS;
	ComPtr<ID3D11VertexShader> OccluderVS;
	ComPtr<ID3D11InputLayout> OccluderInputLayout;
	ComPtr<ID3D11Buffer> HiZParamsCB;
	ComPtr<ID3D11Buffer> CullParamsCB;
	ComPtr<ID3D11Buffer> ViewCB;

	uint32 OccluderCapacity = 0;
	TUniquePtr<FConstantBuffer> OccluderCB;

	uint32 TargetWidth = 0;
	uint32 TargetHeight = 0;
	ComPtr<ID3D11Texture2D> OccluderDepth;
	ComPtr<ID3D11DepthStencilView> OccluderDSV;
	ComPtr<ID3D11ShaderResourceView> OccluderDepthSRV;

	ComPtr<ID3D11Texture2D> HiZ;
	ComPtr<ID3D11ShaderResourceView> HiZSRV;
	std::vector<ComPtr<ID3D11ShaderResourceView>> HiZMipSRVs;
	std::vector<ComPtr<ID3D11UnorderedAccessView>> HiZMipUAVs;
	std::vector<uint32> MipWidths;
	std::vector<uint32> MipHeights;

	std::vector<FTargetSet> SpareTargets;
	uint64 ActiveTargetLastUse = 0;
	uint64 TargetUseCounter = 0;

	uint32 ItemCapacity = 0;
	ComPtr<ID3D11Buffer> ItemBuffer;
	ComPtr<ID3D11ShaderResourceView> ItemSRV;
	ComPtr<ID3D11Buffer> ResultBuffer;
	ComPtr<ID3D11UnorderedAccessView> ResultUAV;
	ComPtr<ID3D11Buffer> ResultStaging;

	std::vector<uint8> ScoreBuckets;
	std::vector<uint8> OccluderLODs;
	std::vector<uint32> HistogramCounts;
	std::vector<float> HistogramScores;
	uint32 HistogramChunks = 0;
	float CoverageScale = 0.0f;
};

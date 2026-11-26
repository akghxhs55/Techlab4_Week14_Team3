#pragma once

#include "Render/Buffer.h"
#include "Render/PipelineState.h"
#include "Render/Decal/ProjectedDecal.h"
#include <span>

class FRenderer;
struct FSceneView;

class FProjectedDecalRenderer
{
public:
	bool Init(FRenderer* InRenderer);
	bool IsReady() const;

	bool OnRenderDepth(
		const FSceneView& View,
		uint32 Width,
		uint32 Height,
		std::span<const FProjectedDecalRenderData> Decals,
		bool bUnlitPass);

private:
	FRenderer* Renderer = nullptr;
	FPipelineState DepthPipelineState;
	TUniquePtr<FConstantBuffer> ConstantBuffer;
	TUniquePtr<FConstantBuffer> DepthViewConstantBuffer;
};

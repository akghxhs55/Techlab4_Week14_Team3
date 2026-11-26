#pragma once

#include "Render/PipelineState.h"
#include "Render/Texture2D.h"
#include "Render/Buffer.h"
#include "Render/SceneView.h"
#include "Render/RenderGraph/RenderGraph.h"

struct alignas(16) FDepthOfFieldData
{
	float InvWidth;
	float InvHeight;
	float ProjectionA;
	float ProjectionB;
	float FocusDistanceMM;
	float FocalLengthMM;
	float SensorHeightMM;
	float ApertureFStop;
	float DepthBlurHalfDistanceCM;
	float DepthBlurRadiusPixels;
	float MaxRadiusPixels;
	float MinimumFStop;
	float DiaphragmBladeCount;
	float SkyDepthCM;
	float NearBlurScale;
	float FarBlurScale;
	float NearMaxRadiusPixels;
	float FarMaxRadiusPixels;
	float Padding[2];
};
static_assert(sizeof(FDepthOfFieldData) == 80);

class FDepthOfFieldRenderer
{
public:
	void Init();
	bool PrepareOutput(FTexture2D* ColorTarget, bool bVisualizeDOF);
	bool AddLayeredPasses(FRenderGraphBuilder& Graph, const FSceneView& View,
		FRenderGraphTextureRef SceneColor, FRenderGraphTextureRef Depth, FRenderGraphTextureRef Output,
		bool bVisualizeDOF);
	bool DidLayeredPassSucceed() const { return bLayeredPassSucceeded; }
	TUniquePtr<FTexture2D>& GetOutputTarget() { return OutputTarget; }

private:
	FPipelineState CoCPipeline;
	FPipelineState TilePipeline;
	FPipelineState GatherPipeline;
	FPipelineState DebugPipeline;
	TUniquePtr<FConstantBuffer> ConstantBuffer;
	TUniquePtr<FTexture2D> OutputTarget;
	bool bLayeredPassSucceeded = true;
	bool DrawLayered(const FPipelineState& Pipeline, const FDepthOfFieldData& Data);
};

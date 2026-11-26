#include "EnginePCH.h"
#include "Render/Decal/ProjectedDecalRenderer.h"

#include "Render/Renderer.h"
#include "Render/RenderCommand.h"
#include "Render/RenderResourceManager.h"
#include "Render/SceneView.h"
#include "Render/Texture2D.h"

bool FProjectedDecalRenderer::Init(FRenderer* InRenderer)
{
	Renderer = InRenderer;
	if (!Renderer)
		return false;

	FPipelineState* DepthPipeline = FRenderResourceManager::GetPSO(EPSOType::DBufferDecal_DepthReconstruction);
	if (!DepthPipeline || !DepthPipeline->Shader || !DepthPipeline->Shader->VertexShader || !DepthPipeline->Shader->PixelShader)
		return false;

	DepthPipelineState = *DepthPipeline;
	ConstantBuffer = RenderCommand::CreateConstantBuffer(sizeof(FProjectedDecalConstants));
	DepthViewConstantBuffer = RenderCommand::CreateConstantBuffer(sizeof(FProjectedDecalDepthViewConstants));

	return IsReady();
}

bool FProjectedDecalRenderer::IsReady() const
{
	return Renderer &&
		DepthPipelineState.Shader &&
		DepthPipelineState.Shader->VertexShader &&
		DepthPipelineState.Shader->PixelShader &&
		ConstantBuffer &&
		ConstantBuffer->GetBuffer() &&
		DepthViewConstantBuffer &&
		DepthViewConstantBuffer->GetBuffer();
}

bool FProjectedDecalRenderer::OnRenderDepth(
	const FSceneView& View,
	uint32 Width,
	uint32 Height,
	std::span<const FProjectedDecalRenderData> Decals,
	bool bUnlitPass)
{
	if (Decals.empty())
		return true;

	if (!IsReady() || Width == 0 || Height == 0)
		return false;

	FProjectedDecalDepthViewConstants ViewConstants{};
	ViewConstants.InverseViewProjection = View.ViewProjectionMatrix.Inverse();
	ViewConstants.ViewportSize = FVector4(
		static_cast<float>(Width),
		static_cast<float>(Height),
		1.0f / static_cast<float>(Width),
		1.0f / static_cast<float>(Height));

	RenderCommand::UpdateBufferData(DepthViewConstantBuffer.get(), &ViewConstants, sizeof(ViewConstants));
	RenderCommand::BindConstantBuffer(5, DepthViewConstantBuffer.get(), EShaderBindFlagBits::Pixel);
	RenderCommand::BindConstantBuffer(4, ConstantBuffer.get(), EShaderBindFlagBits::Pixel);

	RenderCommand::BindPipelineState(DepthPipelineState);
	RenderCommand::BindVertexBuffer(nullptr);
	RenderCommand::BindIndexBuffer(nullptr);

	for (const FProjectedDecalRenderData& Decal : Decals)
	{
		if (Decal.bUnlit != bUnlitPass)
			continue;

		if (!IsValidProjectedDecalRenderData(Decal) || !Decal.ColorTexture || !Decal.ColorTexture->GetSRV())
			continue;

		const FProjectedDecalConstants Constants = MakeProjectedDecalConstants(Decal);
		RenderCommand::UpdateBufferData(ConstantBuffer.get(), &Constants, sizeof(Constants));
		RenderCommand::BindShaderResource(0, Decal.ColorTexture, EShaderBindFlagBits::Pixel);
		RenderCommand::BindShaderResource(2, Decal.NormalTexture, EShaderBindFlagBits::Pixel);
		RenderCommand::BindSamplerState(0, Decal.Sampler, EShaderBindFlagBits::Pixel);

		RenderCommand::Draw(3);
	}

	RenderCommand::BindConstantBuffer(4, nullptr, EShaderBindFlagBits::Pixel);
	RenderCommand::BindConstantBuffer(5, nullptr, EShaderBindFlagBits::Pixel);
	RenderCommand::UnbindShaderResources(0, 1, EShaderBindFlagBits::Pixel);
	RenderCommand::UnbindShaderResources(2, 1, EShaderBindFlagBits::Pixel);

	return true;
}

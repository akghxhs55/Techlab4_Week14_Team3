#include "EnginePCH.h"
#include "Render/PostProcess/DepthOfFieldRenderer.h"
#include "Render/RenderCommand.h"
#include "Render/RenderResourceManager.h"
#include <algorithm>
#include <cmath>

namespace
{
	FDepthOfFieldData MakeDOFData(const FSceneView& View, FTexture2D* ColorTarget)
	{
		const float Width = static_cast<float>(ColorTarget->GetWidth());
		const float Height = static_cast<float>(ColorTarget->GetHeight());
		const FDepthOfFieldSettings& Settings = View.DepthOfField;
		const float SensorHeight = std::max(Settings.SensorWidthMM / std::max(View.AspectRatio, 0.01f), 1.0f);
		const float HalfFOV = FMath::DegreesToRadians(FMath::Clamp(View.FieldOfView, 1.0f, 179.0f)) * 0.5f;
		const float FocalLength = SensorHeight / (2.0f * std::tan(HalfFOV));
		FDepthOfFieldData Data{};
		Data.InvWidth = 1.0f / Width;
		Data.InvHeight = 1.0f / Height;
		Data.ProjectionA = View.ProjectionMatrix.M[0][2];
		Data.ProjectionB = View.ProjectionMatrix.M[3][2];
		Data.FocusDistanceMM = Settings.FocalDistance > 0.0f
			? std::max(Settings.FocalDistance * 10.0f, FocalLength + 0.01f) : 0.0f;
		Data.FocalLengthMM = FocalLength;
		Data.SensorHeightMM = SensorHeight;
		Data.ApertureFStop = std::max(Settings.ApertureFStop, 1.0f);
		// 월드 단위에 맞춰 거리 기준 변환
		Data.DepthBlurHalfDistanceCM = Settings.DepthBlurKmFor50Percent > 0.0f
			? Settings.DepthBlurKmFor50Percent * 100000.0f * CmToWorld : 0.0f;
		Data.DepthBlurRadiusPixels = std::max(Settings.DepthBlurRadius, 0.0f) * Height / 1080.0f;
		Data.NearBlurScale = std::max(Settings.NearBlurScale, 0.0f);
		Data.FarBlurScale = std::max(Settings.FarBlurScale, 0.0f);
		Data.NearMaxRadiusPixels = std::max(Settings.NearMaxBlurRadius, 0.0f) * Height / 1080.0f;
		Data.FarMaxRadiusPixels = std::max(Settings.FarMaxBlurRadius, 0.0f) * Height / 1080.0f;
		Data.MaxRadiusPixels = std::max(Data.NearMaxRadiusPixels, Data.FarMaxRadiusPixels);
		Data.MinimumFStop = std::max(Settings.MinimumFStop, 0.0f);
		Data.DiaphragmBladeCount = static_cast<float>(FMath::Clamp(Settings.DiaphragmBladeCount, 4, 16));
		Data.SkyDepthCM = std::max(View.FarClip, 1.0f);
		return Data;
	}
}

void FDepthOfFieldRenderer::Init()
{
	const auto SetupLayeredPipeline = [](FPipelineState& Pipeline, const char* ShaderPath)
		{
			Pipeline.Shader = FRenderResourceManager::FindShaderProgram(ShaderPath);
			Pipeline.Topology = EPrimitiveTopology::TriangleList;
			Pipeline.RasterizerState = ERasterizerState::SolidNone;
			Pipeline.BlendState = EBlendState::Opaque;
			Pipeline.DepthStencilState = EDepthStencilState::Disabled;
		};
	SetupLayeredPipeline(CoCPipeline, "EngineResources/Shaders/DepthOfFieldCoC.hlsl");
	SetupLayeredPipeline(TilePipeline, "EngineResources/Shaders/DepthOfFieldTile.hlsl");
	SetupLayeredPipeline(GatherPipeline, "EngineResources/Shaders/DepthOfFieldGather.hlsl");
	SetupLayeredPipeline(DebugPipeline, "EngineResources/Shaders/DepthOfFieldDebug.hlsl");
	ConstantBuffer = RenderCommand::CreateConstantBuffer(sizeof(FDepthOfFieldData));
}

bool FDepthOfFieldRenderer::PrepareOutput(FTexture2D* ColorTarget, bool bVisualizeDOF)
{
	const auto IsReady = [](const FPipelineState& Pipeline)
		{
			return Pipeline.Shader && Pipeline.Shader->VertexShader && Pipeline.Shader->PixelShader;
		};
	if (!ColorTarget || !ConstantBuffer || !IsReady(CoCPipeline) ||
		(bVisualizeDOF ? !IsReady(DebugPipeline) :
			(!IsReady(TilePipeline) || !IsReady(GatherPipeline))))
		return false;
	if (!OutputTarget || OutputTarget->GetWidth() != ColorTarget->GetWidth() ||
		OutputTarget->GetHeight() != ColorTarget->GetHeight() ||
		OutputTarget->GetFormat() != ColorTarget->GetFormat())
	{
		FTextureDesc Desc{};
		Desc.Width = ColorTarget->GetWidth();
		Desc.Height = ColorTarget->GetHeight();
		Desc.Format = ColorTarget->GetTextureFormat();
		Desc.BindFlags = ETextureBindFlags::RenderTarget | ETextureBindFlags::ShaderResource;
		auto NewTarget = RenderCommand::CreateTexture2D(Desc);
		if (!NewTarget || !NewTarget->GetRawPtr() || !NewTarget->GetRTV() || !NewTarget->GetSRV())
			return false;
		OutputTarget = std::move(NewTarget);
	}
	return true;
}

bool FDepthOfFieldRenderer::DrawLayered(const FPipelineState& Pipeline, const FDepthOfFieldData& Data)
{
	if (!Pipeline.Shader || !Pipeline.Shader->VertexShader || !Pipeline.Shader->PixelShader || !ConstantBuffer)
		return false;
	RenderCommand::BindPipelineState(Pipeline);
	RenderCommand::BindVertexBuffer(nullptr);
	RenderCommand::BindSamplerState(0, ESamplerState::LinearClamp, EShaderBindFlagBits::Pixel);
	RenderCommand::UpdateBufferData(ConstantBuffer.get(), &Data, sizeof(Data));
	RenderCommand::BindConstantBuffer(0, ConstantBuffer.get(), EShaderBindFlagBits::Pixel);
	RenderCommand::Draw(3);
	return true;
}

bool FDepthOfFieldRenderer::AddLayeredPasses(FRenderGraphBuilder& Graph, const FSceneView& View,
	FRenderGraphTextureRef SceneColor, FRenderGraphTextureRef Depth, FRenderGraphTextureRef Output,
	bool bVisualizeDOF)
{
	bLayeredPassSucceeded = true;
	const auto IsReady = [](const FPipelineState& Pipeline)
		{
			return Pipeline.Shader && Pipeline.Shader->VertexShader && Pipeline.Shader->PixelShader;
		};
	if (!SceneColor || !Depth || !Output || !OutputTarget || !ConstantBuffer ||
		!IsReady(CoCPipeline) ||
		(bVisualizeDOF ? !IsReady(DebugPipeline) :
			(!IsReady(TilePipeline) || !IsReady(GatherPipeline))))
		return false;
	const uint32 Width = OutputTarget->GetWidth();
	const uint32 Height = OutputTarget->GetHeight();
	const FDepthOfFieldData Data = MakeDOFData(View, OutputTarget.get());
	const FRenderGraphTextureRef CoC = Graph.CreateTexture({ Width, Height, ETextureFormat::R16_Float }, "DOFCoC");
	if (!CoC) return false;

	FRenderGraphRasterPassDesc CoCPass{};
	CoCPass.Name = "DOFCoC";
	CoCPass.ReadTextures = { Depth };
	CoCPass.ColorAttachments = { { CoC, ERenderGraphLoadAction::Clear } };
	if (!Graph.AddRasterPass(std::move(CoCPass), [this, Data]
		{
			bLayeredPassSucceeded &= DrawLayered(CoCPipeline, Data);
		})) return false;

	if (bVisualizeDOF)
	{
		FRenderGraphRasterPassDesc DebugPass{};
		DebugPass.Name = "DOFVisualization";
		DebugPass.ReadTextures = { CoC };
		DebugPass.ColorAttachments = { { Output, ERenderGraphLoadAction::Clear } };
		return Graph.AddRasterPass(std::move(DebugPass), [this, Data]
			{
				bLayeredPassSucceeded &= DrawLayered(DebugPipeline, Data);
			});
	}

	const FRenderGraphTextureRef Tiles = Graph.CreateTexture({ (Width + 15) / 16, (Height + 15) / 16,
		ETextureFormat::R16G16_Float }, "DOFTileRanges");
	if (!Tiles) return false;

	FRenderGraphRasterPassDesc TilePass{};
	TilePass.Name = "DOFTileRanges";
	TilePass.ReadTextures = { CoC };
	TilePass.ColorAttachments = { { Tiles, ERenderGraphLoadAction::Clear } };
	if (!Graph.AddRasterPass(std::move(TilePass), [this, Data]
		{
			bLayeredPassSucceeded &= DrawLayered(TilePipeline, Data);
		})) return false;

	FRenderGraphRasterPassDesc GatherPass{};
	GatherPass.Name = "DOFGatherComposite";
	GatherPass.ReadTextures = { SceneColor, Depth, CoC, Tiles };
	GatherPass.ColorAttachments = { { Output, ERenderGraphLoadAction::Clear } };
	return Graph.AddRasterPass(std::move(GatherPass), [this, Data]
		{
			bLayeredPassSucceeded &= DrawLayered(GatherPipeline, Data);
		});
}

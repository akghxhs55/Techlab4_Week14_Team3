#include "EnginePCH.h"
#include "Render/Renderer.h"
#include "Render/RenderCommand.h"
#include <algorithm>
#include <cstring>

#include "Render/PostProcess/HeightFogRenderer.h"
#include "Render/PostProcess/FXAARenderer.h"
#include "Render/PostProcess/DepthOfFieldRenderer.h"
#include "Render/PostProcess/ToneMapData.h"
#include "Render/DecalData.h"

#include "Render/PostProcess/CameraOverlayRenderer.h"
#include "Text/TextRenderer.h"
#include "Render/SkyboxRenderer.h"
#include "Render/Decal/ProjectedDecalRenderer.h"
#include "Render/ParticleCurveTexture.h"
#include "Core/Misc/Paths.h"

namespace
{
	constexpr uint32 CommandSlotBufferBytes = 256;
	constexpr uint32 MinInstanceIdCapacity = 256;

	bool HasResourceSlot(const TArray<FMeshDrawShaderResource>& Resources, uint32 Slot, EShaderBindFlagBits Stages)
	{
		for (const FMeshDrawShaderResource& Entry : Resources)
		{
			if (Entry.Slot == Slot && Entry.Stages == Stages)
				return true;
		}
		return false;
	}

	bool HasSameBoundResource(const TArray<FMeshDrawShaderResource>& Bound, const FMeshDrawShaderResource& Next)
	{
		for (const FMeshDrawShaderResource& Entry : Bound)
		{
			if (Entry.Slot == Next.Slot && Entry.Stages == Next.Stages &&
				Entry.Texture == Next.Texture && Entry.StructuredBuffer == Next.StructuredBuffer)
				return true;
		}
		return false;
	}

	bool HasSameConstantBufferBytes(const FMeshDrawCommand* Prev, const FMeshDrawCommand& Next)
	{
		if (!Prev || Prev->ConstantBuffers.Num() != Next.ConstantBuffers.Num())
			return false;
		for (int32 i = 0; i < Next.ConstantBuffers.Num(); ++i)
		{
			const FMeshDrawConstantBuffer& A = Prev->ConstantBuffers[i];
			const FMeshDrawConstantBuffer& B = Next.ConstantBuffers[i];
			if (A.Slot != B.Slot || A.Stages != B.Stages || A.DataSize != B.DataSize)
				return false;
			if (A.DataSize > 0 && std::memcmp(A.Data.data(), B.Data.data(), A.DataSize) != 0)
				return false;
		}
		return true;
	}
}

FRenderer::FRenderer() = default;
FRenderer::~FRenderer()
{
	FParticleCurveTexture::Get().Shutdown();
}

bool FRenderer::Init()
{
	for (uint32 i = 0; i < MaxCommandSlotCBs; ++i)
	{
		CommandSlotCBs[i] = RenderCommand::CreateConstantBuffer(CommandSlotBufferBytes);
		if (!CommandSlotCBs[i])
			return false;
	}
	ViewCB = RenderCommand::CreateConstantBuffer(sizeof(FViewConstants));
	DrawBatchCB = RenderCommand::CreateConstantBuffer(sizeof(FDrawBatchConstants));
	if (!ViewCB || !DrawBatchCB)
		return false;
	GPUOcclusion.Init();
	FParticleCurveTexture::Get().Init();
	return true;
}

void FRenderer::UploadViewConstants(const FMatrix& ViewProjection, const FMatrix& ViewMatrix,
	const FMatrix& ProjectionMatrix, float TotalTime)
{
	FViewConstants Constants{};
	Constants.VP = ViewProjection;
	Constants.View = ViewMatrix;
	Constants.Projection = ProjectionMatrix;
	const FVector CameraWorldPosition = ViewMatrix.Inverse().TransformPosition(FVector(0.0f, 0.0f, 0.0f));
	Constants.ViewTimeParams = FVector4(TotalTime, CameraWorldPosition.X, CameraWorldPosition.Y, CameraWorldPosition.Z);
	RenderCommand::UpdateBufferData(ViewCB.get(), &Constants, sizeof(Constants));
}

void FRenderer::EnsureInstanceIdCapacity(uint32 Count)
{
	if (Count <= InstanceIdCapacity)
		return;
	uint32 NewCap = std::max(InstanceIdCapacity * 2u, MinInstanceIdCapacity);
	while (NewCap < Count)
		NewCap *= 2u;
	InstanceIdBuffer = RenderCommand::CreateDynamicStructuredBuffer(NewCap, sizeof(uint32));
	InstanceIdCapacity = InstanceIdBuffer ? NewCap : 0;
}

void FRenderer::UnbindUnusedResources(const TArray<FMeshDrawShaderResource>& NextResources,
	TArray<FMeshDrawShaderResource>& InOutBoundResources)
{
	for (const FMeshDrawShaderResource& Prev : InOutBoundResources)
	{
		if (!HasResourceSlot(NextResources, Prev.Slot, Prev.Stages))
			RenderCommand::UnbindShaderResources(Prev.Slot, 1, Prev.Stages);
	}
}

void FRenderer::SubmitVisibleCommands(std::span<const FVisibleMeshDrawCommand> Commands,
	const FStructuredBuffer* GPUSceneBuffer, uint32* OutDrawCalls, uint32* OutParticleDrawCalls)
{
	if (Commands.empty())
		return;

	ScratchInstanceIds.Reset();
	ScratchDrawRuns.Reset();
	ScratchDrawRuns.Reserve(static_cast<int32>(Commands.size()));

	const uint32 TotalCount = static_cast<uint32>(Commands.size());
	for (uint32 i = 0; i < TotalCount; ++i)
	{
		const FVisibleMeshDrawCommand& Vis = Commands[i];
		if (!Vis.Command)
			continue;

		const FMeshDrawCommand& Cmd = *Vis.Command;
		if (Cmd.bUsesGPUScene && Cmd.InstanceCount == 0 && Vis.PrimitiveId != InvalidPrimitiveId)
		{
			const uint32 StartInstance = static_cast<uint32>(ScratchInstanceIds.Num());
			ScratchInstanceIds.Add(Vis.PrimitiveId);
			uint32 RunCount = 1;
			while (i + RunCount < TotalCount && AreVisibleCommandsInstanceCompatible(Vis, Commands[i + RunCount]))
			{
				ScratchInstanceIds.Add(Commands[i + RunCount].PrimitiveId);
				++RunCount;
			}
			ScratchDrawRuns.Add({ &Vis, StartInstance, RunCount, true });
			i += RunCount - 1;
		}
		else
		{
			ScratchDrawRuns.Add({ &Vis, Cmd.StartInstanceLocation, Cmd.InstanceCount, false });
		}
	}

	if (ScratchDrawRuns.IsEmpty())
		return;

	bool bBoundGPUScene = false;
	if (!ScratchInstanceIds.IsEmpty())
	{
		EnsureInstanceIdCapacity(static_cast<uint32>(ScratchInstanceIds.Num()));
		if (InstanceIdBuffer)
		{
			if (void* Mapped = RenderCommand::MapWriteDiscard(InstanceIdBuffer.get()))
			{
				std::memcpy(Mapped, ScratchInstanceIds.GetData(), ScratchInstanceIds.Num() * sizeof(uint32));
				RenderCommand::Unmap(InstanceIdBuffer.get());
			}
			if (GPUSceneBuffer)
				RenderCommand::BindShaderResource(17, GPUSceneBuffer, EShaderBindFlagBits::Vertex | EShaderBindFlagBits::Pixel);
			RenderCommand::BindShaderResource(18, InstanceIdBuffer.get(), EShaderBindFlagBits::Vertex);
			bBoundGPUScene = true;
		}
	}

	RenderCommand::BindConstantBuffer(0, ViewCB.get(), EShaderBindFlagBits::Vertex | EShaderBindFlagBits::Pixel);

	FShaderProgram* PrevShader = nullptr;
	EPrimitiveTopology PrevTopology = static_cast<EPrimitiveTopology>(255);
	ERasterizerState PrevRaster = static_cast<ERasterizerState>(255);
	EBlendState PrevBlend = static_cast<EBlendState>(255);
	EDepthStencilState PrevDepth = static_cast<EDepthStencilState>(255);
	uint32 PrevStencil = 0xFFFFFFFFu;
	const FVertexFactory* PrevVF = nullptr;
	ESamplerState PrevSampler = static_cast<ESamplerState>(255);
	EShaderBindFlagBits PrevSamplerStages = EShaderBindFlagBits::None;
	const FMeshDrawCommand* PrevCBCommand = nullptr;
	TArray<FMeshDrawShaderResource> BoundResources;
	uint32 ParticleDrawCallCount = 0;

	for (const FDrawRun& Run : ScratchDrawRuns)
	{
		const FVisibleMeshDrawCommand& Vis = *Run.Visible;
		const FMeshDrawCommand& Cmd = *Vis.Command;
		if (!Cmd.VertexFactory)
			continue;

		if (Cmd.bIsParticle)
		{
			++ParticleDrawCallCount;
		}

		FShaderProgram* Shader = Vis.ShaderOverride ? Vis.ShaderOverride : Cmd.Pipeline.Shader;
		if (Shader != PrevShader)
		{
			RenderCommand::BindShaderProgram(Shader);
			PrevShader = Shader;
		}
		if (Cmd.Pipeline.Topology != PrevTopology)
		{
			RenderCommand::SetPrimitiveTopology(Cmd.Pipeline.Topology);
			PrevTopology = Cmd.Pipeline.Topology;
		}
		const ERasterizerState Raster = (Vis.bForceWireframe || Cmd.bForceWireframe)
			? ERasterizerState::Wireframe : Cmd.Pipeline.RasterizerState;
		if (Raster != PrevRaster)
		{
			RenderCommand::SetRasterizerState(Raster);
			PrevRaster = Raster;
		}
		const EBlendState Blend = Vis.bHasBlendStateOverride
			? Vis.BlendStateOverride : Cmd.Pipeline.BlendState;
		if (Blend != PrevBlend)
		{
			RenderCommand::SetBlendState(Blend);
			PrevBlend = Blend;
		}
		const EDepthStencilState Depth = Vis.bHasDepthStencilOverride
			? Vis.DepthStencilOverride : Cmd.Pipeline.DepthStencilState;
		const uint32 Stencil = (Vis.bOverrideStencilRef || Cmd.bOverrideStencilRef)
			? (Vis.bOverrideStencilRef ? Vis.StencilRef : Cmd.StencilRef) : Cmd.Pipeline.StencilRef;
		if (Depth != PrevDepth || Stencil != PrevStencil)
		{
			RenderCommand::SetDepthStencilState(Depth, Stencil);
			PrevDepth = Depth;
			PrevStencil = Stencil;
		}

		if (Cmd.VertexFactory != PrevVF)
		{
			Cmd.VertexFactory->Bind();
			PrevVF = Cmd.VertexFactory;
		}

		if (Cmd.Sampler != PrevSampler || Cmd.SamplerStages != PrevSamplerStages)
		{
			RenderCommand::BindSamplerState(0, Cmd.Sampler, Cmd.SamplerStages);
			PrevSampler = Cmd.Sampler;
			PrevSamplerStages = Cmd.SamplerStages;
		}

		UnbindUnusedResources(Cmd.ShaderResources, BoundResources);
		for (const FMeshDrawShaderResource& Res : Cmd.ShaderResources)
		{
			if (HasSameBoundResource(BoundResources, Res))
				continue;
			if (Res.StructuredBuffer)
				RenderCommand::BindShaderResource(Res.Slot, Res.StructuredBuffer, Res.Stages);
			else
				RenderCommand::BindShaderResource(Res.Slot, Res.Texture, Res.Stages);
		}
		BoundResources = Cmd.ShaderResources;

		if (!HasSameConstantBufferBytes(PrevCBCommand, Cmd))
		{
			for (const FMeshDrawConstantBuffer& CB : Cmd.ConstantBuffers)
			{
				if (CB.DataSize > 0 && CB.Slot < MaxCommandSlotCBs && CommandSlotCBs[CB.Slot])
				{
					FConstantBuffer* TargetCB = CommandSlotCBs[CB.Slot].get();
					RenderCommand::UpdateBufferData(TargetCB, CB.Data.data(), CB.DataSize);
					RenderCommand::BindConstantBuffer(CB.Slot, TargetCB, CB.Stages);
				}
			}
			PrevCBCommand = &Cmd;
		}

		if (Run.bGPUSceneInstanced)
		{
			FDrawBatchConstants BatchConstants{};
			BatchConstants.StartInstance = Run.StartInstance;
			BatchConstants.HeatmapWireMode = Vis.HeatmapWireMode;
			RenderCommand::UpdateBufferData(DrawBatchCB.get(), &BatchConstants, sizeof(BatchConstants));
			RenderCommand::BindConstantBuffer(5, DrawBatchCB.get(), EShaderBindFlagBits::Vertex | EShaderBindFlagBits::Pixel);
			RenderCommand::DrawIndexedInstanced(Cmd.IndexCount, Run.InstanceCount,
				Cmd.FirstIndex, Cmd.BaseVertexLocation, 0);
		}
		else if (Run.InstanceCount > 0)
		{
			RenderCommand::DrawIndexedInstanced(Cmd.IndexCount, Run.InstanceCount,
				Cmd.FirstIndex, Cmd.BaseVertexLocation, Run.StartInstance);
		}
		else
		{
			RenderCommand::DrawIndexed(Cmd.IndexCount, Cmd.FirstIndex, Cmd.BaseVertexLocation);
		}
	}

	if (OutDrawCalls)
	{
		*OutDrawCalls += static_cast<uint32>(ScratchDrawRuns.Num());
	}

	if (OutParticleDrawCalls)
	{
		*OutParticleDrawCalls += ParticleDrawCallCount;
	}

	const TArray<FMeshDrawShaderResource> EmptyResources{};
	UnbindUnusedResources(EmptyResources, BoundResources);
	if (bBoundGPUScene)
	{
		RenderCommand::UnbindShaderResources(17, 1, EShaderBindFlagBits::Vertex | EShaderBindFlagBits::Pixel);
		RenderCommand::UnbindShaderResources(18, 1, EShaderBindFlagBits::Vertex);
		RenderCommand::BindConstantBuffer(5, nullptr, EShaderBindFlagBits::Vertex | EShaderBindFlagBits::Pixel);
	}
}

void FRenderer::InitScenePasses()
{
	// 스카이 배경 파노라마(엔진 콘텐츠). 로드에 실패하면 배경은 검정으로 남는다.
	Skybox = MakeUnique<FSkyboxRenderer>();
	if (!Skybox->Init(FPaths::EngineFileUtf8("EngineResources/SkySphere/Sky.hdr")))
	{
		KRS_LOG(Warning, "Skybox panorama not loaded");
		Skybox.reset();
	}
	PreviewSkybox = MakeUnique<FSkyboxRenderer>();
	if (!PreviewSkybox->Init(FPaths::EngineFileUtf8("EngineResources/SkySphere/PreViewer.hdr")))
	{
		KRS_LOG(Warning, "Preview skybox panorama not loaded");
		PreviewSkybox.reset();
	}
	HeightFog = MakeUnique<FHeightFogRenderer>();
	HeightFog->Init(this);
	FXAA = MakeUnique<FFXAARenderer>();
	FXAA->Init(this);
	DepthOfField = MakeUnique<FDepthOfFieldRenderer>();
	DepthOfField->Init();
	CameraOverlay = MakeUnique<FCameraOverlayRenderer>();   // FXAA->Init(this); 아래
	CameraOverlay->Init(this);
	ToneMapConstants = RenderCommand::CreateConstantBuffer(sizeof(FToneMapData));
	DecalConstants = RenderCommand::CreateConstantBuffer(sizeof(FDecalData));
	SceneText = MakeUnique<FTextRenderer>();
	SceneText->Init();
	ProjectedDecal = MakeUnique<FProjectedDecalRenderer>();
	if (!ProjectedDecal->Init(this))
	{
		KRS_LOG(Warning, "ProjectedDecalRenderer init failed");
		ProjectedDecal.reset();
	}
}

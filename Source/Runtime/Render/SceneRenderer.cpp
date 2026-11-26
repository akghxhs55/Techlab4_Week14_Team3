#include "EnginePCH.h"
#include "Render/SceneRenderer.h"
#include "Render/Renderer.h"
#include "Render/SceneLightResources.h"
#include "Render/RenderGraph/RenderGraph.h"
#include "Render/SceneViewTargets.h"
#include "Render/RenderResourceManager.h"
#include "Render/Material.h"
#include "Engine/World.h"
#include "Tasks/Tasks.h"
#include "Core/Stats/LightweightStats.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SceneManagement.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Render/PostProcess/HeightFogRenderer.h"
#include "Render/PostProcess/CameraOverlayRenderer.h"
#include "Render/PostProcess/FXAARenderer.h"
#include "Render/PostProcess/DepthOfFieldRenderer.h"
#include "Render/PostProcess/ToneMapData.h"
#include "Render/DecalData.h"
#include "Render/SkyboxRenderer.h"
#include "Render/ParticleCurveTexture.h"
#include "Text/TextRenderer.h"
#include "UObject/UObjectIterator.h"
#include "Render/GPUStats.h"
#include "Core/EngineTimer.h"
#include "Asset/LOD/StaticMeshLODSelector.h"

#include <cmath>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <utility>
#include "Render/Decal/ProjectedDecalRenderer.h"

DECLARE_CYCLE_STAT("Frustum Cull", STAT_FrustumCull);
DECLARE_CYCLE_STAT("Gather Elements", STAT_GatherElements);
DECLARE_CYCLE_STAT("Decal Pass", STAT_DecalPass);
DECLARE_CYCLE_STAT("Decal Gather Items", STAT_DecalGatherItems);
DECLARE_CYCLE_STAT("Decal Draw", STAT_DecalDraw);

namespace
{
	std::atomic<uint32> GCascadeCount{ 4 };
	std::atomic<float> GCascadeLogRatio{ 0.8f };

	struct FDecalItemRef {
		const FPrimitiveSceneProxy* Proxy;
		uint32 CommandIndex;
	};

	FRenderGraphRasterPassDesc MakeViewportPass(const std::string& Name, FRenderGraphTextureRef Color,
		FRenderGraphTextureRef DepthTarget = {},
		ERenderGraphLoadAction DepthLoad = ERenderGraphLoadAction::Load)
	{
		FRenderGraphRasterPassDesc Pass{};
		Pass.Name = Name;
		Pass.ColorAttachments = { { Color, ERenderGraphLoadAction::Load } };
		if (DepthTarget)
		{
			Pass.DepthAttachment = { DepthTarget, DepthLoad, 1.0f, 0 };
		}
		return Pass;
	}

	bool AddOverlayPass(FRenderGraphBuilder& Graph, const char* Name,
		FRenderGraphTextureRef Color, FRenderGraphTextureRef Depth,
		const std::function<void()>& Draw,
		ERenderGraphLoadAction DepthLoad = ERenderGraphLoadAction::Load)
	{
		return !Draw || Graph.AddRasterPass(MakeViewportPass(Name, Color, Depth, DepthLoad), Draw);
	}

	bool CompareDecalItemRef(const FDecalItemRef& A, const FDecalItemRef& B) {
		return std::less <const FPrimitiveSceneProxy*>()(A.Proxy, B.Proxy);
	}

	bool CompareDecalItemRefProxy(const FDecalItemRef& Ref, const FPrimitiveSceneProxy* Proxy) {
		return std::less <const FPrimitiveSceneProxy*>()(Ref.Proxy, Proxy);
	}
}

bool FSceneRenderer::SetCascadeCount(uint32 Count)
{
	if (Count < 1 || Count > FSceneRenderData::MaxCascadeShadowCount)
		return false;
	GCascadeCount.store(Count, std::memory_order_relaxed);
	return true;
}

uint32 FSceneRenderer::GetCascadeCount()
{
	return GCascadeCount.load(std::memory_order_relaxed);
}

bool FSceneRenderer::SetCascadeLogRatio(float Ratio)
{
	if (!std::isfinite(Ratio) || Ratio < 0.0f || Ratio > 1.0f)
		return false;
	GCascadeLogRatio.store(Ratio, std::memory_order_relaxed);
	return true;
}

float FSceneRenderer::GetCascadeLogRatio()
{
	return GCascadeLogRatio.load(std::memory_order_relaxed);
}

// 생성자
FSceneRenderer::FSceneRenderer(UWorld* InWorld, const FSceneView& InView, FSceneRenderData& InRenderData)
	: World(InWorld), View(InView), RenderData(InRenderData)
{
	CachedViewContext = View.ToViewContext();
}

// 구체와 절두체 평면 교차 판별
bool FSceneRenderer::IntersectSphereFrustum(const FVector4& PositionRadius, const FPlane (&Planes)[6]) const
{
	const float Radius = PositionRadius.W;
	for (const FPlane& Plane : Planes)
	{
		const float Distance = Plane.Normal.X * PositionRadius.X +
			Plane.Normal.Y * PositionRadius.Y +
			Plane.Normal.Z * PositionRadius.Z + Plane.Distance;

		if (Distance < -Radius)
		{
			return false;
		}
	}
	return true;
}

// 아래 두 함수는 UEditorEngine의 같은 이름 함수 본문이다. TargetWorld·SceneView 대신 멤버 World·View를 쓴다.
bool FSceneRenderer::RenderHeightFog(FRenderer* Renderer, FTexture2D* DepthTarget, FTexture2D* ColorTarget)
{
	FHeightFogRenderer* FogRenderer = Renderer->GetHeightFog();
	if (!World || !FogRenderer || !DepthTarget || !ColorTarget || !View.bIsPerspective)
		return false;

	for (TObjectIterator<UExponentialHeightFogComponent> Fog; Fog; ++Fog)
	{
		if (!Fog || !Fog->IsRegistered() || !Fog->IsShown(View.bGameView) || !Fog->GetOwner() ||
			Fog->GetOwner()->GetWorld() != World)
			continue;

		FHeightFogSetting Setting;
		Setting.FogDensity = Fog->GetFogDensity();
		Setting.FogHeightFalloff = Fog->GetFogHeightFalloff();
		Setting.FogHeight = Fog->GetFogHeight();
		Setting.FogColor = Fog->GetFogColor();
		Setting.StartDistance = Fog->GetStartDistance();
		Setting.FogMaxOpacity = Fog->GetFogMaxOpacity();
		Setting.FogCutoffDistance = Fog->GetFogCutoffDistance();
		if (FogRenderer->OnRender(View, DepthTarget, ColorTarget, Setting))
			return true;
	}
	return false;
}

void FSceneRenderer::RenderSceneText(FRenderer* Renderer)
{
	FTextRenderer* TextRenderer = Renderer->GetSceneText();
	if (!World || !TextRenderer) return;
	for (TObjectIterator<UTextRenderComponent> Text; Text; ++Text)
	{
		if (!Text || !Text->IsRegistered() || !Text->GetFont() || !Text->IsShown(View.bGameView) ||
			!Text->GetOwner() || Text->GetOwner()->GetWorld() != World)
			continue;
		TextRenderer->OnRender(Text->GetText(), Text->GetWorldMatrix(), Text->GetTextSize(),
			*Text->GetFont(), View.ViewProjectionMatrix);
	}
}

// 가시성 검사 및 렌더 큐 수집
void FSceneRenderer::InitViews(FRenderer* Renderer, const FSceneRenderOptions& Options)
{
	bLightUploadSucceeded = false;
	if (!World)
	{
		RenderData.ResetFrame();
		return;
	}

	CachedViewContext.bShowSkeletalMesh = Options.bShowSkeletalMesh;
	CachedViewContext.bShowParticles = Options.bShowParticles;

	// 게임 쪽(파티클 LOD 등)이 다음 Tick에서 카메라 거리를 알 수 있게 남긴다
	World->AddRenderedViewLocation(View.ViewLocation);

	FScene& Scene = World->GetScene();
	Scene.FlushStaticCaches(Options);

	// 월드 프리미티브 수집 및 절두체 컬링
	GatherViewVisibilityAndDynamicElements(Scene, RenderData, CachedViewContext, Options, &View.Frustum, Renderer);
	RenderData.AmbientLight = Scene.GetAmbientLight(View.bGameView);

	// 모든 프록시가 같은 FLightRenderParameters를 채우고, 종류는 Type으로만 구분한다.
	auto AddVisibleLight = [this](const FLightSceneProxy* Light)
	{
		FLightRenderParameters Parameters{};
		Light->GetLightShaderParameters(Parameters);
		const int32 Index = RenderData.VisibleLights.Add(Parameters);
		RenderData.VisibleLightProxies.Add(Light);
		if (Light->GetLightComponent() == RenderData.PreviewLight) RenderData.PreviewLightIndex = Index;
	};

	for (const FDirectionalLightSceneProxy* Light : Scene.GetDirectionalLights())
	{
		if (!Light->IsEnabled() || !Light->IsShown(View.bGameView)) continue;
		AddVisibleLight(Light);
	}

	for (const FLocalLightSceneProxy* Light : Scene.GetLocalLights())
	{
		if (!Light->IsEnabled() || !Light->IsShown(View.bGameView)) continue;
		AddVisibleLight(Light);
	}

	RenderData.Commit(bLightUploadSucceeded);

	InitDynamicShadows(Options);
	RenderData.ViewLightResources.CreateShadowDepthAtlasIfNeed(RenderData.ShadowAtlasSliceCount);
	// 그림자 맵이 생긴 광원에 마스크 채널을 주고 광원 데이터를 다시 올린다.
	FinalizeShadowLights(Options);
}

bool FSceneRenderer::BindForwardLighting(FRenderer* Renderer, const FSceneRenderOptions& Options, FTexture2D* ShadowMask)
{
	FSceneLightResources& LightResources = RenderData.ViewLightResources;

	FConstantBuffer* ForwardConstants = LightResources.GetForwardConstants();

	if (!bLightUploadSucceeded || !ForwardConstants)
	{
		return false;
	}
	
	FPipelineState* BasePSO = FRenderResourceManager::GetPSO(EPSOType::StaticMesh_Forward);
	FShaderProgram* Program = FRenderResourceManager::GetForwardShaderProgram(Options.LightingModel);
	if (!BasePSO || !Program)
	{
		return false;
	}

	FForwardLightingConstants Constants{};

	Constants.Ambient = RenderData.AmbientLight;
	Constants.CameraPosition = FVector4(View.ViewLocation.X, View.ViewLocation.Y, View.ViewLocation.Z, 1.0f);

	std::memcpy(Constants.LightCounts, RenderData.LightParameterCounts, sizeof(RenderData.LightParameterCounts));
	std::memcpy(Constants.LightOffsets, RenderData.LightParameterOffsets, sizeof(RenderData.LightParameterOffsets)); // LightOffset은 각 조명별로 시작하는 위치

	RenderCommand::UpdateBufferData(ForwardConstants, &Constants, sizeof(Constants));
	RenderCommand::BindConstantBuffer(1, ForwardConstants, EShaderBindFlagBits::Vertex | EShaderBindFlagBits::Pixel);
	// 광원 채널(ShadowMapChannel)로 읽는 그림자 마스크. 없으면 셰이더가 그림자 없음(1)으로 본다.
	RenderCommand::BindShaderResource(13, ShadowMask, EShaderBindFlagBits::Pixel);

	FForwardLightTileGrid& Tiles = LightResources.GetTiles();
	Tiles.PreparePixelShader(Renderer, bTilesBuilt);
	RenderCommand::BindConstantBuffer(3, Tiles.GetPixelConstants(), EShaderBindFlagBits::Pixel);
	const std::array<FStructuredBuffer*, 3> TileBuffers = Tiles.GetPixelResources();
	RenderCommand::BindShaderResource(9, TileBuffers[0], EShaderBindFlagBits::Pixel);
	RenderCommand::BindShaderResource(10, TileBuffers[1], EShaderBindFlagBits::Pixel);
	RenderCommand::BindShaderResource(12, TileBuffers[2], EShaderBindFlagBits::Pixel);
	RenderCommand::BindShaderResource(8, RenderData.HasLights() && LightResources.GetSceneLights() && LightResources.GetSceneLights()->GetSRV() ? LightResources.GetSceneLights() : nullptr, EShaderBindFlagBits::Vertex | EShaderBindFlagBits::Pixel);

	RenderCommand::BindSamplerState(1, ESamplerState::LinearClamp, EShaderBindFlagBits::Vertex | EShaderBindFlagBits::Pixel);

	return true;
}

void FSceneRenderer::UnbindForwardLighting(FRenderer* Renderer)
{
	RenderCommand::UnbindShaderResources(9, 2, EShaderBindFlagBits::Pixel);
	RenderCommand::BindConstantBuffer(3, nullptr, EShaderBindFlagBits::Pixel);
	RenderCommand::UnbindShaderResources(12, 2, EShaderBindFlagBits::Vertex | EShaderBindFlagBits::Pixel);
	RenderData.ViewLightResources.GetTiles().ResetPixelState();
	RenderCommand::UnbindShaderResources(8, 1, EShaderBindFlagBits::Vertex | EShaderBindFlagBits::Pixel);
	RenderCommand::UnbindShaderResources(11, 1, EShaderBindFlagBits::Vertex | EShaderBindFlagBits::Pixel);
}

void FSceneRenderer::BuildVisiblePassCommands(const FScene& Scene, EMeshPass Pass, const FSceneRenderOptions& Options,
	bool bWireframe, TArray<FVisibleMeshDrawCommand>& OutCommands) const
{
	OutCommands.Reset();
	const TArray<FCachedMeshDrawEntry>& CachedList = Scene.GetCachedDrawList(Pass);
	const TArray<FMeshDrawCommand>& DynamicList = RenderData.DynamicPassCommands[static_cast<size_t>(Pass)];
	OutCommands.Reserve(CachedList.Num() + DynamicList.Num());

	const uint32 NumPrimitives = static_cast<uint32>(RenderData.PrimitiveVisibilityMap.Num());
	const bool bApplyWireframe = bWireframe && (Pass == EMeshPass::BasePass);
	const bool bApplyUnlit = (Options.LightingModel == ELightingModel::Unlit) && (Pass == EMeshPass::BasePass);

	for (const FCachedMeshDrawEntry& Entry : CachedList)
	{
		const uint32 PrimIdx = Entry.PrimitiveIndex;
		if (PrimIdx >= NumPrimitives || !RenderData.PrimitiveVisibilityMap[PrimIdx])
			continue;
		if (Pass == EMeshPass::ShadowDepth)
		{
			if (Entry.LODIndex != 0)
				continue;
		}
		else if (PrimIdx < static_cast<uint32>(RenderData.PrimitiveSelectedLODs.Num()) &&
			RenderData.PrimitiveSelectedLODs[PrimIdx] != Entry.LODIndex)
		{
			continue;
		}

		FVisibleMeshDrawCommand Vis{};
		Vis.Command = &Entry.Command;
		Vis.PrimitiveId = Entry.Command.PrimitiveId;
		Vis.StateBucketId = Entry.Command.StateBucketId;
		Vis.SortKey = Entry.Command.SortKey;
		Vis.bForceWireframe = bApplyWireframe;

		if (bApplyUnlit)
			Vis.ShaderOverride = ResolveUnlitShaderOverride(Entry.Command, Options);
		if (Pass == EMeshPass::Translucency && Entry.Command.Proxy)
		{
			const FAABB& Bounds = Entry.Command.Proxy->GetBounds();
			const FVector Delta = Bounds.Center - View.ViewLocation;
			Vis.SortKey = MakeTranslucentSortKey(Delta.Dot(Delta));
		}
		OutCommands.Add(Vis);
	}

	for (const FMeshDrawCommand& DynCmd : DynamicList)
	{
		FVisibleMeshDrawCommand Vis{};
		Vis.Command = &DynCmd;
		Vis.PrimitiveId = DynCmd.PrimitiveId;
		Vis.StateBucketId = DynCmd.StateBucketId;
		Vis.SortKey = DynCmd.SortKey;
		Vis.bForceWireframe = bApplyWireframe && !DynCmd.bIsParticle;
		if (bApplyUnlit)
			Vis.ShaderOverride = ResolveUnlitShaderOverride(DynCmd, Options);
		OutCommands.Add(Vis);
	}

	std::stable_sort(OutCommands.begin(), OutCommands.end(),
		[](const FVisibleMeshDrawCommand& A, const FVisibleMeshDrawCommand& B) { return A.SortKey < B.SortKey; });
}

bool FSceneRenderer::AddWorldNormalPass(FRenderGraphBuilder& Graph, FRenderer* Renderer,
	FSceneViewTargets& Targets, const FSceneRenderOptions& Options)
{
	const FRenderGraphTextureRef SceneColor = Graph.ImportTexture(Targets.SceneColor.get(), "SceneColor");
	const FRenderGraphTextureRef Depth = Graph.ImportTexture(Targets.Depth.get(), "SceneDepth");
	if (!SceneColor || !Depth) return false;

	FRenderGraphRasterPassDesc Pass{};
	Pass.Name = "WorldNormal";
	Pass.ColorAttachments = { { SceneColor, ERenderGraphLoadAction::Clear, { 0.0f, 0.0f, 0.0f, 1.0f } } };
	Pass.DepthAttachment = { Depth, ERenderGraphLoadAction::Clear, 1.0f, 0 };
	return Graph.AddRasterPass(std::move(Pass), [this, Renderer, bDrawPrimitives = Options.bDrawPrimitives, Options]
		{
			if (!bDrawPrimitives || !World) return;
			TArray<FMeshDrawCommand>& DynList = RenderData.DynamicPassCommands[static_cast<size_t>(EMeshPass::WorldNormal)];
			DynList.Reset();
			FWorldNormalPassMeshProcessor Processor;
			for (const FMeshBatch& Batch : RenderData.Collector.GetMeshBatches())
			{
				if (!Processor.AddMeshBatch(Batch, Options, &RenderData.Collector, DynList))
				{
					bCorePassSucceeded = false;
					return;
				}
			}

			BuildVisiblePassCommands(World->GetScene(), EMeshPass::WorldNormal, Options, false, RenderData.VisibleWorldNormalCommands);
			Renderer->UploadViewConstants(View.ViewProjectionMatrix, View.ViewMatrix, View.ProjectionMatrix,
				static_cast<float>(EngineTimer::GetTotalTime()));
			Renderer->SubmitVisibleCommands(RenderData.VisibleWorldNormalCommands, RenderData.GPUSceneBuffer);
		});
}

bool FSceneRenderer::AddCorePasses(FRenderGraphBuilder& Graph, FRenderer* Renderer, FSceneViewTargets& Targets,
	const FSceneRenderOptions& Options)
{
	if (!World || !Renderer || !Targets.SceneColor || !Targets.Depth)
		return false;
	const uint32 Width = Targets.SceneColor->GetWidth();
	const uint32 Height = Targets.SceneColor->GetHeight();
	if (!Width || !Height || !Targets.IsValidFor(Width, Height))
		return false;

	bCorePassSucceeded = true;
	bTilesBuilt = false;
	if (Options.bShowWorldNormal)
		return AddWorldNormalPass(Graph, Renderer, Targets, Options);

	const FRenderGraphTextureRef SceneColor = Graph.ImportTexture(Targets.SceneColor.get(), "SceneColor"); // Texture Handle 리턴 해줌
	const FRenderGraphTextureRef Depth = Graph.ImportTexture(Targets.Depth.get(), "SceneDepth");
	if (!SceneColor || !Depth)
		return false;

	const FRenderGraphTextureDesc HDRDesc{ Width, Height, ETextureFormat::R16G16B16A16_Float };
	const FRenderGraphTextureRef LightingHDR = Graph.CreateTexture(HDRDesc, "LightingHDR");
	if (!LightingHDR)
		return false;

	// 스카이 배경을 물체보다 먼저 HDR 타깃에 깐다. 이 패스가 LightingHDR을 Clear하고, 포워드 패스는 Load로 그 위에 그린다.
	FSkyboxRenderer* PreviewSkybox = Renderer->GetPreviewSkybox();
	FSkyboxRenderer* Skybox = (Options.bUsePreviewSkybox && PreviewSkybox && PreviewSkybox->IsValid())
		? PreviewSkybox
		: Renderer->GetSkybox();
	// 직교 뷰에서는 스카이를 그리지 않는다.
	const bool bDrawSkybox = Skybox && Skybox->IsValid() && View.bIsPerspective;
	if (bDrawSkybox)
	{
		FRenderGraphRasterPassDesc Sky{};
		Sky.Name = "Skybox";
		Sky.ColorAttachments = {
			{ LightingHDR, ERenderGraphLoadAction::Clear, { 0.0f, 0.0f, 0.0f, 1.0f } }
		};
		if (!Graph.AddRasterPass(std::move(Sky), [this, Skybox]
			{
				if (bCorePassSucceeded)
					Skybox->OnRender(View.ViewProjectionMatrix, View.ViewLocation);
			})) return false;
	}

	// 할당된 그림자 맵마다 Caster 깊이를 아틀라스에 그린다.
	FTexture2D* ShadowDepthAtlas = RenderData.ViewLightResources.GetShadowDepthAtlas();
	const bool bAnyShadowAllocated = std::any_of(RenderData.ProjectedShadows.begin(), RenderData.ProjectedShadows.end(),
		[](const TUniquePtr<FProjectedShadowInfo>& Shadow) { return Shadow->bAllocated; });
	FRenderGraphTextureRef ShadowAtlasRef{};   // Projection 패스가 읽는다
	if (ShadowDepthAtlas && bAnyShadowAllocated)
	{
		const FRenderGraphTextureRef AtlasRef = Graph.ImportTexture(ShadowDepthAtlas, "ShadowDepthAtlas");
		if (!AtlasRef) return false;
		ShadowAtlasRef = AtlasRef;

		FRenderGraphRasterPassDesc ShadowDepths{};
		ShadowDepths.Name = "ShadowDepths";
		ShadowDepths.DepthAttachment = { AtlasRef, ERenderGraphLoadAction::Clear, 1.0f, 0 };   // 단계 8에서 Load로
		if (!Graph.AddRasterPass(std::move(ShadowDepths), [this, Renderer, &Graph, AtlasRef]
			{
				SCOPED_GPU_STAT(GPUStats::STAT_GPU_ShadowDepths);
				RenderShadowDepthMaps(Renderer, static_cast<FTexture2DArray*>(Graph.Resolve(AtlasRef)));
			})) return false;
	}

	ProjectedDecals.Reset();
	{ // Opaque forward rendering
		RenderData.VisibleBasePassCommands.Reset();
		if (Options.bDrawPrimitives)
		{
			BuildVisiblePassCommands(World->GetScene(), EMeshPass::BasePass, Options, View.bIsWireframe, RenderData.VisibleBasePassCommands);
		}
		// 현재 업로드된 광원 정보를 기존 그리드에 전달한다.
		FForwardLightTileGrid& Tiles = RenderData.ViewLightResources.GetTiles();
		FStructuredBuffer* SceneLights = RenderData.ViewLightResources.GetSceneLights();
		FForwardLightingConstants GridLighting{};
		std::memcpy(GridLighting.LightCounts, RenderData.LightParameterCounts, sizeof(RenderData.LightParameterCounts));
		std::memcpy(GridLighting.LightOffsets, RenderData.LightParameterOffsets, sizeof(RenderData.LightParameterOffsets));

		// 픽셀 조명 경로에서만 목록을 사용한다.
		const ELightCullingMode CullingMode = !View.bIsWireframe && Options.LightingModel != ELightingModel::Gouraud &&
			Options.LightingModel != ELightingModel::Unlit ? Options.LightCullingMode : ELightCullingMode::Off;

		// Off도 이번 프레임의 화면 정보와 입력 광원 개수를 갱신한다.
		bool bUseDepthPrepass = false;
		const bool bGridPrepared = Options.bDrawPrimitives && bLightUploadSucceeded &&
			Tiles.PrepareBuild(View, GridLighting, SceneLights, Targets.Depth.get(), CullingMode);

		// 투영 데칼 수집 및 정렬
		bool bHasLitProjectedDecal = false;
		bool bHasUnlitProjectedDecal = false;
		if (Options.bDrawDecals && Renderer->GetProjectedDecal() && Renderer->GetProjectedDecal()->IsReady() && World)
		{
			const FScene& Scene = World->GetScene();
			ProjectedDecals.Reserve(Scene.Decals.Num());
			for (UDecalComponent* DecalComp : Scene.Decals)
			{
				if (!DecalComp || !DecalComp->IsShown(View.bGameView))
					continue;
				FProjectedDecalRenderData Data{};
				if (!DecalComp->TryGetRenderData(Data))
					continue;
				bHasLitProjectedDecal |= !Data.bUnlit;
				bHasUnlitProjectedDecal |= Data.bUnlit;
				ProjectedDecals.Add(Data);
			}
			std::stable_sort(ProjectedDecals.begin(), ProjectedDecals.end(),
				[](const FProjectedDecalRenderData& A, const FProjectedDecalRenderData& B)
				{
					if (A.SortOrder != B.SortOrder)
						return A.SortOrder < B.SortOrder;
					return A.ComponentID < B.ComponentID;
				});
		}
		const bool bHasProjectedDecals = !ProjectedDecals.IsEmpty();

		const bool bHasGpuParticles = Options.bDrawPrimitives && !RenderData.Collector.GetGpuParticleBatches().IsEmpty();
		const bool bNeedsLightCulling = bGridPrepared && CullingMode != ELightCullingMode::Off && (RenderData.HasSpotLights() || RenderData.HasPointLights());
		// 그림자 마스크는 장면 깊이로 Projection하므로 프리패스가 필요하다. Gouraud는 FinalizeShadowLights가 채널을 주지 않는다.
		const bool bForwardShadows = Options.bDrawPrimitives && ShadowAtlasRef && !ShadowedLights.IsEmpty();
		if (bNeedsLightCulling || bForwardShadows || bHasGpuParticles || bHasProjectedDecals)
		{
			FPipelineState* DepthPipeline = FRenderResourceManager::GetPSO(EPSOType::StaticMesh_DepthOnly);
			if (!DepthPipeline || !DepthPipeline->Shader || !DepthPipeline->Shader->PixelShader) return false;
			bUseDepthPrepass = true;

			// 기존 프리패스를 유지해 ForwardOpaque에서 깊이를 재사용한다.
			FRenderGraphRasterPassDesc Prepass{};
			Prepass.Name = "DepthPrepass";
			Prepass.DepthAttachment = { Depth, ERenderGraphLoadAction::Clear, 1.0f, 0 };
			if (!Graph.AddRasterPass(std::move(Prepass), [this, Renderer, Options]
				{
					if (!bCorePassSucceeded || !World) return;
					BuildVisiblePassCommands(World->GetScene(), EMeshPass::DepthPass, Options, false, RenderData.VisibleDepthPassCommands);
					if (RenderData.VisibleDepthPassCommands.IsEmpty()) return;

					SCOPED_GPU_STAT(GPUStats::STAT_GPU_DepthPrepass);
					Renderer->UploadViewConstants(View.ViewProjectionMatrix, View.ViewMatrix, View.ProjectionMatrix,
						static_cast<float>(EngineTimer::GetTotalTime()));
					Renderer->SubmitVisibleCommands(RenderData.VisibleDepthPassCommands, RenderData.GPUSceneBuffer);
				})) return false;
		}

		// 그림자 광원마다 배정된 채널(R~A)에 Projection한다. 한 픽셀에서 겹치는 그림자 광원은 서로 다른 채널을 받는다.
		FRenderGraphTextureRef ForwardShadowMask{};
		if (bForwardShadows)
		{
			ForwardShadowMask = Graph.CreateTexture({ Width, Height, ETextureFormat::R8G8B8A8_UNorm }, "ScreenShadowMask");
			if (!ForwardShadowMask) return false;

			FRenderGraphRasterPassDesc Projection{};
			Projection.Name = "ShadowProjection";
			Projection.ReadTextures = { Depth, ShadowAtlasRef };   // t0 장면 깊이, t1 깊이 아틀라스
			Projection.ColorAttachments = { { ForwardShadowMask, ERenderGraphLoadAction::Clear, { 1.0f, 1.0f, 1.0f, 1.0f } } };
			if (!Graph.AddRasterPass(std::move(Projection), [this, Width, Height]
				{
					if (!bCorePassSucceeded) return;
					for (const FShadowedLight& Light : ShadowedLights)
						RenderShadowProjectionsForLight(RenderData.VisibleLightInfos[Light.LightId], Light.Channel, Width, Height);
					RenderCommand::BindConstantBuffer(0, nullptr, EShaderBindFlagBits::Pixel);
				})) return false;
		}

		if (bNeedsLightCulling)
		{
			// 선택한 알고리즘에 맞는 컴퓨트 패스를 등록한다.
			FRenderGraphComputePassDesc Culling{};
			Culling.Name = "TileLightCulling";
			Culling.ReadResources = { SceneLights->GetBuffer() };

			const std::array<ID3D11Resource*, 4> Writes = Tiles.GetWriteResources();
			Culling.WriteResources.assign(Writes.begin(), Writes.end());
			if (!Graph.AddComputePass(std::move(Culling),
				[this, Renderer, SceneLights, DepthTexture = Targets.Depth.get()]
				{
					if (bCorePassSucceeded)
						bTilesBuilt = RenderData.ViewLightResources.GetTiles().ExecuteBuild(Renderer, DepthTexture, SceneLights);
				})) 
				return false;
		}
		if (bHasGpuParticles)
		{
			FRenderGraphComputePassDesc GpuParticleSim{};
			GpuParticleSim.Name = "GpuParticleSimulate";
			if (Targets.Depth && Targets.Depth->GetRawPtr())
			{
				GpuParticleSim.ReadResources.push_back(Targets.Depth->GetRawPtr());
			}
			for (const FGpuParticleBatch& Batch : RenderData.Collector.GetGpuParticleBatches())
			{
				if (!Batch.GpuResources || Batch.MaxParticles == 0)
				{
					continue;
				}
				FGpuParticleEmitterResources& Res = *Batch.GpuResources;
				if (!Res.ParticleBuffer || Res.MaxParticles != Batch.MaxParticles)
				{
					Res.ParticleBuffer = RenderCommand::CreateRWStructuredBuffer(Batch.ParticleStride, Batch.MaxParticles);
					Res.MaxParticles = Batch.MaxParticles;
					if (Res.ParticleBuffer)
					{
						RenderCommand::ClearUnorderedAccess(Res.ParticleBuffer.get(), 0);
					}
				}
				else if (Batch.bResetRequested && Res.ParticleBuffer)
				{
					RenderCommand::ClearUnorderedAccess(Res.ParticleBuffer.get(), 0);
				}
				if (Res.ParticleBuffer && Res.ParticleBuffer->GetBuffer())
				{
					ID3D11Resource* RawRes = Res.ParticleBuffer->GetBuffer();
					if (std::find(GpuParticleSim.WriteResources.begin(), GpuParticleSim.WriteResources.end(), RawRes) == GpuParticleSim.WriteResources.end())
					{
						GpuParticleSim.WriteResources.push_back(RawRes);
					}
				}
			}
			if (!GpuParticleSim.WriteResources.empty())
			{
				if (!Graph.AddComputePass(std::move(GpuParticleSim), [this, DepthTexture = Targets.Depth.get()]
				{
					FComputeShader* InjectCS = FRenderResourceManager::GetComputeShader("EngineResources/Shaders/GpuParticleSimulate.hlsl", "CSInjectSpawns");
					FComputeShader* SimCS = FRenderResourceManager::GetComputeShader("EngineResources/Shaders/GpuParticleSimulate.hlsl", "CSSimulate");
					if (!InjectCS || !SimCS)
					{
						return;
					}
					FParticleCurveTexture& CurveAtlas = FParticleCurveTexture::Get();
					CurveAtlas.FlushPendingUploads();

					const bool bDepthReady = DepthTexture && DepthTexture->GetSRV();
					const FMatrix ViewProj = View.ViewProjectionMatrix;
					const FMatrix InvViewProj = View.ViewProjectionMatrix.Inverse();
					const FVector4 ScreenAndClip(
						static_cast<float>(View.ViewRect.Width),
						static_cast<float>(View.ViewRect.Height),
						View.NearClip,
						View.FarClip);

					for (const FGpuParticleBatch& Batch : RenderData.Collector.GetGpuParticleBatches())
					{
						if (!Batch.GpuResources || Batch.MaxParticles == 0)
						{
							continue;
						}
						FGpuParticleEmitterResources& Res = *Batch.GpuResources;
						if (!Res.ParticleBuffer)
						{
							continue;
						}
						if (!Res.SimCB)
						{
							Res.SimCB = RenderCommand::CreateConstantBuffer(sizeof(FGpuParticleSimParams));
						}
						if (!Res.SimCB)
						{
							continue;
						}

						FGpuParticleSimParams SimParams = Batch.SimParams;
						SimParams.ViewProj = ViewProj;
						SimParams.InvViewProj = InvViewProj;
						SimParams.ScreenAndClip = ScreenAndClip;
						if (!bDepthReady)
						{
							SimParams.HasCollision = 0;
						}
						RenderCommand::UpdateBufferData(Res.SimCB.get(), &SimParams, sizeof(SimParams));

						if (Batch.SpawnCount > 0 && !Batch.PendingSpawnBytes.IsEmpty())
						{
							if (!Res.SpawnUploadBuffer || Res.SpawnCapacity < Batch.SpawnCount)
							{
								const uint32 NewCap = std::max(Batch.SpawnCount, 64u);
								Res.SpawnUploadBuffer = RenderCommand::CreateDynamicStructuredBuffer(NewCap, Batch.ParticleStride);
								Res.SpawnCapacity = Res.SpawnUploadBuffer ? NewCap : 0;
							}
							if (Res.SpawnUploadBuffer)
							{
								if (void* MappedSpawn = RenderCommand::MapWriteDiscard(Res.SpawnUploadBuffer.get()))
								{
									std::memcpy(MappedSpawn, Batch.PendingSpawnBytes.GetData(), Batch.PendingSpawnBytes.Num());
									RenderCommand::Unmap(Res.SpawnUploadBuffer.get());
								}
								RenderCommand::BindComputeShader(InjectCS);
								RenderCommand::BindConstantBuffer(0, Res.SimCB.get(), EShaderBindFlagBits::Compute);
								RenderCommand::BindShaderResource(0, Res.SpawnUploadBuffer.get(), EShaderBindFlagBits::Compute);
								RenderCommand::BindUnorderedAccess(0, Res.ParticleBuffer.get());
								RenderCommand::Dispatch((Batch.SpawnCount + 63u) / 64u, 1, 1);
								RenderCommand::UnbindShaderResources(0, 1, EShaderBindFlagBits::Compute);
							}
						}

						RenderCommand::BindComputeShader(SimCS);
						RenderCommand::BindConstantBuffer(0, Res.SimCB.get(), EShaderBindFlagBits::Compute);
						RenderCommand::BindShaderResource(1, CurveAtlas.GetTexture(), EShaderBindFlagBits::Compute);
						RenderCommand::BindShaderResource(2, bDepthReady ? DepthTexture : nullptr, EShaderBindFlagBits::Compute);
						RenderCommand::BindSamplerState(0, ESamplerState::LinearClamp, EShaderBindFlagBits::Compute);
						RenderCommand::BindUnorderedAccess(0, Res.ParticleBuffer.get());
						RenderCommand::Dispatch((Batch.MaxParticles + 255u) / 256u, 1, 1);

						RenderCommand::UnbindUnorderedAccess(0);
						RenderCommand::UnbindShaderResources(1, 2, EShaderBindFlagBits::Compute);
						RenderCommand::BindComputeShader(nullptr);
					}
				}))
					return false;
			}
		}
		FRenderGraphTextureRef DBufferLit{};
		FRenderGraphTextureRef DBufferUnlit{};
		FRenderGraphTextureRef DBufferNormal{};
		if (bUseDepthPrepass && bHasProjectedDecals)
		{
			DBufferLit = Graph.CreateTexture({ Width, Height, ETextureFormat::R8G8B8A8_UNorm }, "DBufferLit");
			DBufferUnlit = Graph.CreateTexture({ Width, Height, ETextureFormat::R8G8B8A8_UNorm }, "DBufferUnlit");
			DBufferNormal = Graph.CreateTexture({ Width, Height, ETextureFormat::R16G16B16A16_Float }, "DBufferNormal");
			if (!DBufferLit || !DBufferUnlit || !DBufferNormal) return false;

			FRenderGraphRasterPassDesc DBufferLitPass{};
			DBufferLitPass.Name = "DBufferDecal_Lit";
			DBufferLitPass.ReadTextures = { Depth };
			DBufferLitPass.ReadTextureStartSlot = 1;
			DBufferLitPass.ColorAttachments = {
				{ DBufferLit, ERenderGraphLoadAction::Clear, { 0.0f, 0.0f, 0.0f, 0.0f } },
				{ DBufferNormal, ERenderGraphLoadAction::Clear, { 0.0f, 0.0f, 0.0f, 0.0f } }
			};
			if (!Graph.AddRasterPass(std::move(DBufferLitPass), [this, Renderer, Width, Height, bHasLitProjectedDecal]
			{
				if (bHasLitProjectedDecal && Renderer->GetProjectedDecal())
				{
					Renderer->GetProjectedDecal()->OnRenderDepth(View, Width, Height, ProjectedDecals, false);
				}
			})) return false;

			FRenderGraphRasterPassDesc DBufferUnlitPass{};
			DBufferUnlitPass.Name = "DBufferDecal_Unlit";
			DBufferUnlitPass.ReadTextures = { Depth };
			DBufferUnlitPass.ReadTextureStartSlot = 1;
			DBufferUnlitPass.ColorAttachments = {
				{ DBufferUnlit, ERenderGraphLoadAction::Clear, { 0.0f, 0.0f, 0.0f, 0.0f } }
			};
			if (!Graph.AddRasterPass(std::move(DBufferUnlitPass), [this, Renderer, Width, Height, bHasUnlitProjectedDecal]
			{
				if (bHasUnlitProjectedDecal && Renderer->GetProjectedDecal())
				{
					Renderer->GetProjectedDecal()->OnRenderDepth(View, Width, Height, ProjectedDecals, true);
				}
			})) return false;
		}

		FRenderGraphRasterPassDesc Forward{};
		Forward.Name = "ForwardOpaque";
		if (DBufferLit && DBufferUnlit && DBufferNormal)
		{
			Forward.ReadTextures = { DBufferLit, DBufferUnlit, DBufferNormal };
			Forward.ReadTextureStartSlot = 14;
		}
		Forward.ColorAttachments = {
			{ LightingHDR, bDrawSkybox ? ERenderGraphLoadAction::Load : ERenderGraphLoadAction::Clear, { 0.0f, 0.0f, 0.0f, 1.0f } }
		};
		Forward.DepthAttachment = { Depth, bUseDepthPrepass ? ERenderGraphLoadAction::Load : ERenderGraphLoadAction::Clear, 1.0f, 0 };
		if (!Graph.AddRasterPass(std::move(Forward), [this, Renderer, &Graph, LightingOptions = Options, bUseDepthPrepass, ForwardShadowMask]
		{
			if (!LightingOptions.bDrawPrimitives) return;
			bCorePassSucceeded = BindForwardLighting(Renderer, LightingOptions, ForwardShadowMask ? Graph.Resolve(ForwardShadowMask) : nullptr);
			if (!bCorePassSucceeded) return;
			Renderer->UploadViewConstants(View.ViewProjectionMatrix, View.ViewMatrix, View.ProjectionMatrix,
				static_cast<float>(EngineTimer::GetTotalTime()));
			if (bUseDepthPrepass)
			{
				for (FVisibleMeshDrawCommand& Vis : RenderData.VisibleBasePassCommands)
				{
					const UMaterial* BaseMat = (Vis.Command && Vis.Command->Material) ? Vis.Command->Material->GetBaseMaterial() : nullptr;
					if (Vis.Command && !Vis.Command->bIsParticle && (!BaseMat || !BaseMat->bAlphaMasked))
					{
						Vis.bHasDepthStencilOverride = true;
						Vis.DepthStencilOverride = EDepthStencilState::PrepassReadOnly;
					}
				}
			}
			{
				SCOPED_GPU_STAT(GPUStats::STAT_GPU_ForwardOpaque);
				Renderer->SubmitVisibleCommands(RenderData.VisibleBasePassCommands, RenderData.GPUSceneBuffer,
					&RenderData.RenderStats.DrawCalls, &RenderData.RenderStats.ParticleDrawCalls);

				// 언리얼 본 가중치 히트맵: 활성화된 스켈레탈 메시 위에 와이어프레임 오버레이를 한 번 더 그린다.
				if (!View.bIsWireframe)
				{
					TArray<FVisibleMeshDrawCommand> HeatmapWireCommands;
					for (const FVisibleMeshDrawCommand& SrcVis : RenderData.VisibleBasePassCommands)
					{
						if (SrcVis.Command && SrcVis.Command->DebugWeightBone >= 0)
						{
							FVisibleMeshDrawCommand WireVis = SrcVis;
							WireVis.HeatmapWireMode = 1;
							WireVis.bForceWireframe = true;
							WireVis.bHasBlendStateOverride = true;
							WireVis.BlendStateOverride = EBlendState::AlphaBlend;
							WireVis.bHasDepthStencilOverride = true;
							WireVis.DepthStencilOverride = EDepthStencilState::PrepassReadOnly;
							HeatmapWireCommands.Add(std::move(WireVis));
						}
					}
					if (!HeatmapWireCommands.IsEmpty())
					{
						Renderer->SubmitVisibleCommands(HeatmapWireCommands, RenderData.GPUSceneBuffer);
					}
				}
			}

			UnbindForwardLighting(Renderer);
		})) return false;

		if (Options.bDrawDecals)
		{
			FRenderGraphRasterPassDesc Decal{};
			Decal.Name = "Decal";
			Decal.ColorAttachments = { { LightingHDR, ERenderGraphLoadAction::Load,{ 0.0f, 0.0f, 0.0f, 1.0f } } };
			Decal.DepthAttachment = { Depth, ERenderGraphLoadAction::Load, 1.0f, 0 };
			Decal.ReadTextures = {};
			Decal.Viewport = {};
			if (!Graph.AddRasterPass(std::move(Decal), [this, Renderer, bDecalCulling = Options.bDecalCulling,
				bDecalNarrowCulling = Options.bDecalNarrowCulling, bDecalBVHCulling = Options.bDecalBVHCulling,
				bDecalOBBTree = Options.bDecalOBBTree]
				{
					SCOPE_CYCLE_COUNTER(STAT_DecalPass);
					if (!bCorePassSucceeded || RenderData.VisibleBasePassCommands.IsEmpty()) return;

					FPipelineState* DecalPipeline = FRenderResourceManager::GetPSO(EPSOType::Decal);
					FPipelineState* DecalAdditivePipeline = FRenderResourceManager::GetPSO(EPSOType::DecalAdditive);
					if (!DecalPipeline || !DecalPipeline->Shader) return;
					if (!DecalAdditivePipeline || !DecalAdditivePipeline->Shader) return;

					// 여기부터 패스 끝까지가 GPU 측정 구간이다. 위의 조기 return은 잴 것이 없으므로 제외한다.
					SCOPED_GPU_STAT(GPUStats::STAT_GPU_Decal);

					FConstantBuffer* Constants = Renderer->GetDecalConstants();
					FDecalData Data{};

					const FScene& Scene = World->GetScene();
					RenderData.RenderStats.DecalCount = Scene.Decals.Num();

					TArray<UDecalComponent*> SortedDecals = Scene.Decals;
					std::sort(SortedDecals.begin(), SortedDecals.end(), CompareDecalSortOrder);

					TArray<FDecalItemRef> ItemRefs;
					ItemRefs.Reserve(RenderData.VisibleBasePassCommands.Num());

					for (uint32 CmdIndex = 0; CmdIndex < static_cast<uint32>(RenderData.VisibleBasePassCommands.Num()); ++CmdIndex)
					{
						const FVisibleMeshDrawCommand& Vis = RenderData.VisibleBasePassCommands[CmdIndex];
						const FMeshDrawCommand* Cmd = Vis.Command;
						if (!Cmd || !Cmd->bReceiveDecal || !Cmd->Proxy || Cmd->bIsParticle || !Cmd->VertexFactory)
							continue;
						if (!FRenderResourceManager::ResolveShader(DecalPipeline->Shader, Cmd->VertexFactory->GetType()))
							continue;
						ItemRefs.Add({ Cmd->Proxy, CmdIndex });
					}

					std::sort(ItemRefs.begin(), ItemRefs.end(), CompareDecalItemRef);

					Renderer->UploadViewConstants(View.ViewProjectionMatrix, View.ViewMatrix, View.ProjectionMatrix,
						static_cast<float>(EngineTimer::GetTotalTime()));

					TArray<FMeshDrawCommand> DecalCmdStorage;
					TArray<FVisibleMeshDrawCommand> DecalVisList;

					for (UDecalComponent* Decal : SortedDecals)
					{
						if (Decal->GetDecalTexture() == nullptr) continue;
						RenderData.RenderStats.DecalsRendered++;

						Decal->SetDecalViewProj();
						Data.Fade = Decal->GetFade();
						Data.bAdditive = Decal->IsAdditive() ? 1.0f : 0.0f;
						Data.VP = Decal->GetDecalVP();

						RenderCommand::UpdateBufferData(Constants, &Data, sizeof(Data));
						RenderCommand::BindConstantBuffer(1, Constants, EShaderBindFlagBits::Pixel);

						DecalCmdStorage.Reset();
						DecalVisList.Reset();
						const FAABB DecalAABB = Decal->GetWorldAABB();
						const FOBB DecalOBB = Decal->GetWorldOBB();
						const FBox DecalBox{ DecalAABB.Center - DecalAABB.Extent, DecalAABB.Center + DecalAABB.Extent };
						const FPipelineState& BasePSO = Decal->IsAdditive() ? *DecalAdditivePipeline : *DecalPipeline;

						std::function<void(const FDecalItemRef&)> ConsiderItem = [&](const FDecalItemRef& Ref)
						{
							const FVisibleMeshDrawCommand& SrcVis = RenderData.VisibleBasePassCommands[Ref.CommandIndex];
							const FMeshDrawCommand* SrcCmd = SrcVis.Command;
							const FPrimitiveSceneProxy* Proxy = Ref.Proxy;
							const FAABB& ItemBounds = Proxy->GetBounds();
							RenderData.RenderStats.DecalItemsTested++;

							if (!bDecalBVHCulling && bDecalCulling)
							{
								RenderData.RenderStats.DecalBroadTests++;
								if (!IntersectAABB(DecalAABB, ItemBounds)) return;
							}

							RenderData.RenderStats.DecalNarrowTested++;
							if (bDecalNarrowCulling && !IntersectOBBAABB(DecalOBB, ItemBounds)) return;

							if (bDecalNarrowCulling && bDecalOBBTree && Proxy->GetMesh())
							{
								const FStaticMeshData& MeshData = Proxy->GetMesh()->GetMeshData();
								const FMatrix LocalToWorld = Proxy->GetLocalToWorld();
								if (MeshData.OBBTree)
								{
									RenderData.RenderStats.DecalOBBTreeTested++;
									if (!MeshData.OBBTree->Intersects(DecalOBB, LocalToWorld)) return;
								}
							}

							FShaderProgram* ResolvedShader = FRenderResourceManager::ResolveShader(BasePSO.Shader, SrcCmd->VertexFactory->GetType());
							if (!ResolvedShader) return;

							FMeshDrawCommand& Cmd = DecalCmdStorage.AddDefaulted_GetRef();
							Cmd.Pipeline = BasePSO;
							Cmd.Pipeline.Shader = ResolvedShader;
							Cmd.VertexFactory = SrcCmd->VertexFactory;
							Cmd.FirstIndex = SrcCmd->FirstIndex;
							Cmd.IndexCount = SrcCmd->IndexCount;
							Cmd.BaseVertexLocation = SrcCmd->BaseVertexLocation;
							Cmd.InstanceCount = SrcCmd->InstanceCount;
							Cmd.StartInstanceLocation = SrcCmd->StartInstanceLocation;
							Cmd.Sampler = ESamplerState::LinearClamp;
							Cmd.SamplerStages = EShaderBindFlagBits::Pixel;
							Cmd.ShaderResources.Add({ 0u, EShaderBindFlagBits::Pixel, Decal->GetDecalTexture(), nullptr });
							Cmd.PrimitiveId = SrcVis.PrimitiveId;
							Cmd.bUsesGPUScene = SrcCmd->bUsesGPUScene;
							Cmd.bSkeletalMesh = SrcCmd->bSkeletalMesh;
							Cmd.ObjectKey = SrcCmd->ObjectKey;
							Cmd.Proxy = SrcCmd->Proxy;
							Cmd.StateBucketId = ComputeMeshCommandStateBucket(Cmd);
							Cmd.SortKey = MakeOpaqueSortKey(Cmd.StateBucketId, 0, 0, 0);
						};

						{
							SCOPE_CYCLE_COUNTER(STAT_DecalGatherItems);
							if (bDecalBVHCulling)
							{
								Scene.BVH.Query(
									[&](const FBox& Bounds)
									{
										RenderData.RenderStats.DecalBroadTests++;
										return UWorld::AreAABBsOverlapping(DecalBox, Bounds);
									},
									[&](FPrimitiveSceneProxy* Proxy)
									{
										FDecalItemRef* Begin = ItemRefs.GetData();
										FDecalItemRef* End = Begin + ItemRefs.Num();
										FDecalItemRef* it = std::lower_bound(Begin, End, Proxy, CompareDecalItemRefProxy);
										while (it != End && it->Proxy == Proxy)
										{
											ConsiderItem(*it);
											++it;
										}
									});
							}
							else
							{
								for (const FDecalItemRef& Ref : ItemRefs)
								{
									ConsiderItem(Ref);
								}
							}
						}

						if (DecalCmdStorage.IsEmpty()) continue;

						DecalVisList.Reserve(DecalCmdStorage.Num());
						for (const FMeshDrawCommand& Cmd : DecalCmdStorage)
						{
							FVisibleMeshDrawCommand Vis{};
							Vis.Command = &Cmd;
							Vis.PrimitiveId = Cmd.PrimitiveId;
							Vis.StateBucketId = Cmd.StateBucketId;
							Vis.SortKey = Cmd.SortKey;
							DecalVisList.Add(Vis);
						}

						std::stable_sort(DecalVisList.begin(), DecalVisList.end(),
							[](const FVisibleMeshDrawCommand& A, const FVisibleMeshDrawCommand& B) { return A.SortKey < B.SortKey; });

						{
							SCOPE_CYCLE_COUNTER(STAT_DecalDraw);
							Renderer->SubmitVisibleCommands(DecalVisList, RenderData.GPUSceneBuffer);
						}
						RenderData.RenderStats.DecalDrawCalls += static_cast<uint32>(DecalVisList.Num());
					}
					RenderCommand::BindConstantBuffer(1, nullptr, EShaderBindFlagBits::Pixel);
				})) return false;
		}
	}

	

	FRenderGraphRasterPassDesc ToneMap{};
	ToneMap.Name = "ToneMap";
	ToneMap.ReadTextures = { LightingHDR };
	ToneMap.ColorAttachments = { { SceneColor, ERenderGraphLoadAction::Clear, { 0.0f, 0.0f, 0.0f, 1.0f } } };
	if (!Graph.AddRasterPass(std::move(ToneMap), [this, Renderer, &Graph, LightingHDR, bGamma = Options.bGammaCorrection, DisplayGamma = Options.DisplayGamma]
		{
			if (bCorePassSucceeded)
			{
				if (FPipelineState* Pipeline = FRenderResourceManager::GetPSO(EPSOType::ToneMap);
					Pipeline && Pipeline->Shader && Pipeline->Shader->VertexShader && Pipeline->Shader->PixelShader)
				{
					FToneMapData Data{};
					Data.InvGamma = 1.0f / std::max(DisplayGamma, 0.01f);
					Data.bGammaCorretion = bGamma ? 1.0f : 0.0f;
					FConstantBuffer* Constants = Renderer->GetToneMapConstants();
					RenderCommand::UpdateBufferData(Constants, &Data, sizeof(Data));

					RenderCommand::BindPipelineState(*Pipeline);
					RenderCommand::BindConstantBuffer(0, Constants, EShaderBindFlagBits::Pixel);
					RenderCommand::BindVertexBuffer(nullptr);
					RenderCommand::Draw(3);
				}
			}
		})) return false;

	return true;
}

bool FSceneRenderer::AddViewportPasses(FRenderGraphBuilder& Graph, FRenderer* Renderer,
	FSceneViewTargets& Targets, const FSceneRenderOptions& Options,
	const FScenePassCallbacks& Callbacks) // 뷰포트에 그려져야 되는 패스들을 옵션에 따라 정해진 순서로 Graph에 등록
{
	FSceneRenderOptions CoreOptions = Options;
	if (Options.bShowLightCulling)
	{
		// 디버그 보기에서 현재 뷰의 실제 타일 목록을 생성한다.
		CoreOptions.LightingModel = ELightingModel::Lambert;
	}
	if (!AddCorePasses(Graph, Renderer, Targets, CoreOptions)) // 기본적으로 그려져야 되는 Core Pass 들
		return false;
	
	const FRenderGraphTextureRef SceneColor = Graph.ImportTexture(Targets.SceneColor.get(), "SceneColor");
	const FRenderGraphTextureRef Depth = Graph.ImportTexture(Targets.Depth.get(), "SceneDepth");
	if (!SceneColor || !Depth) return false;
	const bool bVisualizeDOF = Options.bVisualizeDOF && Callbacks.DepthOfFieldOutput;
	bHasFog = false;
	bFXAAPassSucceeded = true;

	if (!Options.bShowWorldNormal && Options.bDrawPrimitives && Options.bEnableFog && Callbacks.HeightFog)
	{
		FRenderGraphRasterPassDesc Pass = MakeViewportPass("HeightFog", SceneColor);
		Pass.ReadTextures = { Depth };
		if (!Graph.AddRasterPass(std::move(Pass), [this, DrawFog = Callbacks.HeightFog]
			{
			bHasFog = DrawFog();
			})) return false;
	}
	if (!Options.bShowWorldNormal && Options.bDrawPrimitives)
	{
		if (!Graph.AddRasterPass(MakeViewportPass("Translucent", SceneColor, Depth),
			[this, Renderer, GetFog = Callbacks.TranslucentFogConstants]
			{
			RenderTranslucent(Renderer, GetFog ? GetFog(bHasFog) : nullptr);
			})) return false;
	}
	FRenderGraphTextureRef ActiveColor = SceneColor;
	if (Callbacks.DepthOfFieldOutput)
	{
		ActiveColor = Graph.ImportTexture(Callbacks.DepthOfFieldOutput, "DepthOfFieldOutput");
		if (!ActiveColor) return false;
		FDepthOfFieldRenderer* DOF = Renderer->GetDepthOfField();
		if (!DOF || !DOF->AddLayeredPasses(Graph, View, SceneColor, Depth, ActiveColor,
			Options.bVisualizeDOF)) return false;
	}
	if (!Options.bShowWorldNormal && !bVisualizeDOF && Callbacks.SceneText &&
		!Graph.AddRasterPass(MakeViewportPass("SceneText", ActiveColor, Depth), Callbacks.SceneText))
		return false;

	if (!Options.bShowWorldNormal && Options.bShowSceneDepth && Callbacks.DepthVisualization)
	{
		FRenderGraphRasterPassDesc Pass = MakeViewportPass("SceneDepthVisualization", ActiveColor);
		Pass.ReadTextures = { Depth };
		if (!Graph.AddRasterPass(std::move(Pass), Callbacks.DepthVisualization)) return false;
	}

	if (!Options.bShowWorldNormal && !Options.bShowSceneDepth && !bVisualizeDOF &&
		View.NeedsCameraOverlay(Options.bShowVignette, Options.bShowCameraOverlay))
	{
		const uint32 Width = Targets.SceneColor->GetWidth();
		const uint32 Height = Targets.SceneColor->GetHeight();
		if (!Graph.AddRasterPass(MakeViewportPass("CameraOverlay", ActiveColor),
			[this, Renderer, Width, Height,
			bVignette = Options.bShowVignette, bOverlay = Options.bShowCameraOverlay]
			{
				if (FCameraOverlayRenderer* Overlay = Renderer->GetCameraOverlay())
					Overlay->OnRender(View, Width, Height, bVignette, bOverlay);
			})) return false;
	}

	FRenderGraphTextureRef FinalColor = ActiveColor;
	if (!Options.bShowWorldNormal && Callbacks.FXAAOutput && Callbacks.FXAA)
	{
		FinalColor = Graph.ImportTexture(Callbacks.FXAAOutput, "FXAAOutput");
		if (!FinalColor) return false;
		FRenderGraphRasterPassDesc Pass = MakeViewportPass("FXAA", FinalColor);
		Pass.ReadTextures = { ActiveColor };
		if (!Graph.AddRasterPass(std::move(Pass), [this, ApplyFXAA = Callbacks.FXAA]
			{
			bFXAAPassSucceeded = ApplyFXAA();
			})) return false;
	}

	if (Options.bShowLightCulling && Options.bDrawPrimitives && !Options.bShowWorldNormal && !bVisualizeDOF)
	{
		FRenderGraphRasterPassDesc Debug = MakeViewportPass("LightCullingDebug", FinalColor);
		Debug.ReadTextures = { Depth };
		if (!Graph.AddRasterPass(std::move(Debug), [this, Renderer, &Graph, Depth]
			{
				if (!bCorePassSucceeded) return;
				FPipelineState* Pipeline = FRenderResourceManager::GetPSO(EPSOType::LightCullingDebug);
				if (!Pipeline || !Pipeline->Shader || !Pipeline->Shader->VertexShader || !Pipeline->Shader->PixelShader)
				{
					bCorePassSucceeded = false;
					return;
				}
				FForwardLightTileGrid& Tiles = RenderData.ViewLightResources.GetTiles();
				Tiles.PreparePixelShader(Renderer, bTilesBuilt);
				RenderCommand::BindConstantBuffer(3, Tiles.GetPixelConstants(), EShaderBindFlagBits::Pixel);
				RenderCommand::BindShaderResource(9, Tiles.GetPixelResources()[0], EShaderBindFlagBits::Pixel);
				RenderCommand::BindPipelineState(*Pipeline);
				RenderCommand::BindVertexBuffer(nullptr);
				RenderCommand::Draw(3);
				RenderCommand::UnbindShaderResources(9, 1, EShaderBindFlagBits::Pixel);
				RenderCommand::BindConstantBuffer(3, nullptr, EShaderBindFlagBits::Pixel);
				Tiles.ResetPixelState();
			})) return false;
	}

	const FSceneOverlayCallbacks& Overlay = Callbacks.Overlays;
	if (Options.bShowCascade && Callbacks.CascadeVisualization)
	{
		FRenderGraphRasterPassDesc Pass = MakeViewportPass("CascadeFloorVisualization", FinalColor);
		Pass.ReadTextures = { Depth };
		if (!Graph.AddRasterPass(std::move(Pass), Callbacks.CascadeVisualization)) return false;
	}
	if (!AddOverlayPass(Graph, "EditorLines", FinalColor, Depth, Overlay.EditorLines) ||
		!AddOverlayPass(Graph, "EditorGrid", FinalColor, Depth, Overlay.EditorGrid) ||
		!AddOverlayPass(Graph, "EditorOutline", FinalColor, Depth, Overlay.EditorOutline) ||
		!AddOverlayPass(Graph, "EditorGizmo", FinalColor, Depth, Overlay.EditorGizmo, ERenderGraphLoadAction::Clear))
		return false;
	if (Overlay.bClearEditorDepth)
	{
		FRenderGraphRasterPassDesc ClearDepth{};
		ClearDepth.Name = "EditorClearDepth";
		ClearDepth.DepthAttachment = { Depth, ERenderGraphLoadAction::Clear, 1.0f, 0 };
		if (!Graph.AddRasterPass(std::move(ClearDepth), [] {})) return false;
	}
	if (!AddOverlayPass(Graph, "EditorUUID", FinalColor, Depth, Overlay.EditorUUID)) return false;
	if (!AddOverlayPass(Graph, "GameUI", FinalColor, Depth, Overlay.GameUI)) return false;
	return true;
}


bool FSceneRenderer::Render(FRenderer* Renderer, FSceneViewTargets& Targets, const FSceneRenderOptions& Options, const std::function<void(FScenePassCallbacks&)>& ExtraPasses)
{
	FTexture2D* ColorTarget = Targets.SceneColor.get();
	FTexture2D* DepthTarget = Targets.Depth.get();
	if (!World || !Renderer || !ColorTarget || !DepthTarget)
		return false;
	const uint32 Width = ColorTarget->GetWidth();
	const uint32 Height = ColorTarget->GetHeight();
	if (!Width || !Height || !Targets.IsValidFor(Width, Height))
		return false;

	FScenePassCallbacks Callbacks;
	Callbacks.HeightFog = [this, Renderer, DepthTarget, ColorTarget]
		{
			return RenderHeightFog(Renderer, DepthTarget, ColorTarget);
		};
	Callbacks.TranslucentFogConstants = [Renderer](bool bHasFog)
		{
			FHeightFogRenderer* Fog = Renderer->GetHeightFog();
			return Fog ? Fog->GetTranslucentFogConstants(bHasFog) : nullptr;
		};
	Callbacks.SceneText = [this, Renderer] { RenderSceneText(Renderer); };
	FDepthOfFieldRenderer* DOF = Renderer->GetDepthOfField();
	const FDepthOfFieldSettings& DOFSettings = View.DepthOfField;
	const bool bUseDOF = Options.bEnableDOF && DOFSettings.bEnabled && View.bIsPerspective &&
		!View.bIsWireframe && !Options.bShowSceneDepth && !Options.bShowWorldNormal &&
		((DOFSettings.FocalDistance > 0.0f && DOFSettings.ApertureFStop > 0.0f) ||
			(DOFSettings.DepthBlurRadius > 0.0f && DOFSettings.DepthBlurKmFor50Percent > 0.0f)) &&
		DOF && DOF->PrepareOutput(ColorTarget, Options.bVisualizeDOF);
	if (bUseDOF)
	{
		Callbacks.DepthOfFieldOutput = DOF->GetOutputTarget().get();
	}

	FFXAARenderer* FXAA = Renderer->GetFXAA();
	FTexture2D* PostDOFColor = bUseDOF ? DOF->GetOutputTarget().get() : ColorTarget;
	const bool bUseFXAA = Options.bEnableFXAA && !Options.bShowSceneDepth && !(bUseDOF && Options.bVisualizeDOF) &&
		!Options.bShowWorldNormal && FXAA && FXAA->PrepareOutput(PostDOFColor);
	if (bUseFXAA)
	{
		Callbacks.FXAAOutput = FXAA->GetOutputTarget().get();
		Callbacks.FXAA = [FXAA, PostDOFColor] { return FXAA->OnRender(PostDOFColor); };
	}

	if (ExtraPasses) ExtraPasses(Callbacks);

	FRenderGraphBuilder Graph;
	if (!AddViewportPasses(Graph, Renderer, Targets, Options, Callbacks) ||
		!Graph.Execute() || !DidCorePassSucceed() ||
		(bUseDOF && !DOF->DidLayeredPassSucceed()) || !DidFXAAPassSucceed())
		return false;
	if (bUseFXAA && !Targets.SwapSceneColorAndBind(FXAA->GetOutputTarget()))
		return false;
	if (!bUseFXAA && bUseDOF && !Targets.SwapSceneColorAndBind(DOF->GetOutputTarget()))
		return false;

	RenderCommand::EndRenderPass();
	return true;
}

// 반투명 렌더링
void FSceneRenderer::RenderTranslucent(FRenderer* Renderer, FConstantBuffer* FogConstants)
{
	if (!Renderer || !World)
	{
		return;
	}

	RenderCommand::BindConstantBuffer(3, FogConstants, EShaderBindFlagBits::Pixel);
	BuildVisiblePassCommands(World->GetScene(), EMeshPass::Translucency, FSceneRenderOptions{}, View.bIsWireframe, RenderData.VisibleTranslucencyCommands);
	Renderer->UploadViewConstants(View.ViewProjectionMatrix, View.ViewMatrix, View.ProjectionMatrix,
		static_cast<float>(EngineTimer::GetTotalTime()));
	Renderer->SubmitVisibleCommands(RenderData.VisibleTranslucencyCommands, RenderData.GPUSceneBuffer,
		&RenderData.RenderStats.DrawCalls, &RenderData.RenderStats.ParticleDrawCalls);
	RenderCommand::BindConstantBuffer(3, nullptr, EShaderBindFlagBits::Pixel);
}

void FSceneRenderer::GatherViewVisibilityAndDynamicElements(FScene& Scene, FSceneRenderData& Data,
	const FViewContext& View, const FSceneRenderOptions& Options,
	const FFrustumPlanes* Frustum, FRenderer* Renderer)
{
	Data.ResetFrame();
	FMeshElementCollector& Collector = Data.Collector;
	FRenderStats& RenderStats = Data.RenderStats;
	TArray<FPrimitiveSceneProxy*>& VisibleProxies = Data.VisibleProxies;

	const uint32 NumPrimitives = Scene.GetNumPrimitives();
	Data.PrimitiveVisibilityMap.Init(uint8(0), NumPrimitives);
	Data.PrimitiveOccludedMap.Init(uint8(0), NumPrimitives);
	Data.PrimitiveSelectedLODs.Init(uint8(0), NumPrimitives);

	{
		SCOPE_CYCLE_COUNTER(STAT_FrustumCull);
		// 표시 상태는 프록시 캐시로 판정해 드로우 수집 전에 제외한다.
		std::function<void(FPrimitiveSceneProxy*)> Visit = [&](FPrimitiveSceneProxy* Proxy)
		{
			if (!Proxy->IsShown(View.bGameView))
				return;
			if (Proxy->IsSkinned() && !View.bShowSkeletalMesh)
				return;
			VisibleProxies.Add(Proxy);
		};

		if (Frustum)
		{
			Scene.BVH.QueryCull(
				FrustumAllPlanesMask,
				[Frustum](const FBox& Bounds, uint32& Mask)
				{
					return static_cast<EBVHCullResult>(ClassifyBoxInFrustum(Bounds, *Frustum, Mask));
				},
				Visit);
		}
		else
		{
			Scene.BVH.QueryCull(0, [](const FBox&, uint32&) { return EBVHCullResult::Inside; }, Visit);
		}
	}

	RenderStats.TotalPrimitives = NumPrimitives;

	const uint8* OccludedMask = nullptr;
	if (Renderer && Renderer->GetGPUOcclusion().GetSettings().bEnabled)
	{
		FGPUOcclusion& Occlusion = Renderer->GetGPUOcclusion();
		if (Occlusion.Run(VisibleProxies.GetData(), static_cast<uint32>(VisibleProxies.Num()), View))
		{
			const std::vector<uint8>& Occluded = Occlusion.GetOccluded();
			if (Occlusion.GetSettings().bCull)
			{
				uint32 Kept = 0;
				for (uint32 i = 0; i < static_cast<uint32>(VisibleProxies.Num()); ++i)
				{
					if (!Occluded[i])
						VisibleProxies[Kept++] = VisibleProxies[i];
				}
				VisibleProxies.SetNum(Kept);
			}
			else
			{
				OccludedMask = Occluded.data();
			}
		}
	}

	RenderStats.VisiblePrimitives = VisibleProxies.Num();

	{
		SCOPE_CYCLE_COUNTER(STAT_GatherElements);

		Data.LODInputs.Reset();
		Data.LODInputs.Reserve(VisibleProxies.Num());
		for (const FPrimitiveSceneProxy* Proxy : VisibleProxies)
		{
			Data.LODInputs.Add({ Proxy ? Proxy->GetLODSphere() : FLODSphere{}, Proxy ? Proxy->GetRenderState() : nullptr });
		}
		SelectLODs(Data.LODInputs, View, Data.SelectedLODs);

		for (uint32 VisibleIndex = 0; VisibleIndex < static_cast<uint32>(VisibleProxies.Num()); ++VisibleIndex)
		{
			FPrimitiveSceneProxy* Proxy = VisibleProxies[VisibleIndex];
			if (!Proxy || !Proxy->IsVisible())
				continue;
			if (Proxy->IsSkinned() && !View.bShowSkeletalMesh)
				continue;

			const uint8 LOD = (VisibleIndex < static_cast<uint32>(Data.SelectedLODs.Num()))
				? Data.SelectedLODs[VisibleIndex] : 0;

			if (Proxy->GetPackedIndex() >= 0 && static_cast<uint32>(Proxy->GetPackedIndex()) < NumPrimitives)
			{
				const uint32 PrimIdx = static_cast<uint32>(Proxy->GetPackedIndex());
				Data.PrimitiveVisibilityMap[PrimIdx] = 1;
				Data.PrimitiveSelectedLODs[PrimIdx] = LOD;
				if (OccludedMask && OccludedMask[VisibleIndex])
				{
					Data.PrimitiveOccludedMap[PrimIdx] = 1;
				}
			}

			if (Proxy->GetRenderState() && Proxy->GetLODCount() > 0)
			{
				const uint32 ClampedLOD = std::min(static_cast<uint32>(LOD), Proxy->GetLODCount() - 1u);
				if (ClampedLOD < 4)
				{
					++RenderStats.LODCounts[ClampedLOD];
					const FCachedMeshLOD& CachedLOD = Proxy->GetLOD(ClampedLOD);
					for (uint32 s = 0; s < CachedLOD.NumSections; ++s)
					{
						const uint64 Tris = Proxy->GetSection(CachedLOD.FirstSection + s).IndexCount / 3;
						RenderStats.LODTriangles[ClampedLOD] += Tris;
						RenderStats.Triangles += Tris;
					}
				}
			}
			else if (const FMeshRenderState* State = Proxy->GetRenderState())
			{
				for (uint32 s = 0; s < static_cast<uint32>(State->Sections.Num()); ++s)
				{
					RenderStats.Triangles += State->Sections[s].IndexCount / 3;
				}
			}

			if (Proxy->HasDynamicRelevance())
			{
				Proxy->GetDynamicMeshElements(View, Collector, LOD);
			}
		}

		TArray<FMeshBatch>& DynamicBatches = Collector.GetMeshBatchesMutable();
		for (FMeshBatch& Batch : DynamicBatches)
		{
			if (Batch.bIsParticle || !Batch.bUsesGPUScene)
				continue;

			for (FMeshBatchElement& Element : Batch.Elements)
			{
				if (Element.WorldMatrix == nullptr && Element.Proxy && !Element.Proxy->IsSkinned() && Element.Proxy->GetPackedIndex() >= 0)
				{
					Element.PrimitiveId = static_cast<uint32>(Element.Proxy->GetPackedIndex());
				}
				else
				{
					const FMatrix& Model = Element.WorldMatrix ? *Element.WorldMatrix
						: (Element.Proxy ? Element.Proxy->GetLocalToWorld() : FMatrix::Identity);
					const float BoneOffset = Element.DebugWeightBone >= 0 ? static_cast<float>(Element.DebugWeightBone + 1) : 0.0f;
					const uint32 DynIdx = NumPrimitives + static_cast<uint32>(Data.DynamicPrimitiveData.Num());
					Data.DynamicPrimitiveData.Add(MakePrimitiveSceneData(Model, Element.bReceiveShadow, Element.bReceiveDecal, BoneOffset));
					Element.PrimitiveId = DynIdx;
				}
			}
		}

		Collector.CommitGpuResources();
		Data.GPUSceneBuffer = Scene.SyncGPUScene(Data.DynamicPrimitiveData);

		const FBasePassMeshProcessor BaseProcessor;
		const FDepthPassMeshProcessor DepthProcessor;
		const FTranslucencyPassMeshProcessor TranslucencyProcessor;
		const FShadowDepthMeshProcessor ShadowProcessor;

		for (const FMeshBatch& Batch : Collector.GetMeshBatches())
		{
			BaseProcessor.AddMeshBatch(Batch, Options, &Collector, Data.DynamicPassCommands[static_cast<size_t>(EMeshPass::BasePass)]);
			DepthProcessor.AddMeshBatch(Batch, Options, &Collector, Data.DynamicPassCommands[static_cast<size_t>(EMeshPass::DepthPass)]);
			TranslucencyProcessor.AddMeshBatch(Batch, Options, &Collector, Data.DynamicPassCommands[static_cast<size_t>(EMeshPass::Translucency)]);
			ShadowProcessor.AddMeshBatch(Batch, Options, &Collector, Data.DynamicPassCommands[static_cast<size_t>(EMeshPass::ShadowDepth)]);
		}
	}
}


bool FSceneRenderer::CompareDecalSortOrder(const UDecalComponent* A, const UDecalComponent* B) {
	return A->GetSortOrder() < B->GetSortOrder();
}

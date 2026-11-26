#include "EnginePCH.h"
#include "Scene.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/AmbientLightComponent.h"
#include "Render/Material.h"
#include <algorithm>

namespace
{
	class FSceneStaticPDI : public FStaticPrimitiveDrawInterface
	{
	public:
		TArray<FStaticMeshBatch> Batches;

		void DrawMesh(const FStaticMeshBatch& Batch) override
		{
			Batches.Add(Batch);
		}
	};

	void AppendCachedPassCommands(
		EMeshPass Pass,
		const FMeshPassProcessor& Processor,
		const FStaticMeshBatch& Batch,
		const FSceneRenderOptions& Options,
		uint32 PrimitiveIndex,
		std::array<TArray<FCachedMeshDrawEntry>, static_cast<size_t>(EMeshPass::Num)>& CachedDrawLists,
		TArray<TArray<FCachedCommandSlotRef>>& ProxyCachedCommands)
	{
		TArray<FMeshDrawCommand> Built;
		if (!Processor.AddMeshBatch(Batch, Options, nullptr, Built))
			return;
		TArray<FCachedMeshDrawEntry>& List = CachedDrawLists[static_cast<size_t>(Pass)];
		for (FMeshDrawCommand& Cmd : Built)
		{
			Cmd.PrimitiveId = PrimitiveIndex;
			const uint32 OwnerSlot = static_cast<uint32>(ProxyCachedCommands[PrimitiveIndex].Num());
			const uint32 EntryIdx = static_cast<uint32>(List.Num());
			List.Add({ std::move(Cmd), PrimitiveIndex, OwnerSlot, Batch.LODIndex });
			ProxyCachedCommands[PrimitiveIndex].Add({ Pass, EntryIdx });
		}
	}
}

void FScene::AddPrimitive(UPrimitiveComponent* Component)
{
	if (!Component || Component->SceneProxy) return;

	FPrimitiveSceneProxy* Proxy = Component->CreateSceneProxy();
	if (!Proxy) return;

	const uint32 Index = static_cast<uint32>(PrimitiveSceneProxies.Num());
	Proxy->Scene = this;
	Proxy->PackedIndex = static_cast<int32>(Index);
	Proxy->SetPrimitiveId(Index);
	Component->SceneProxy = Proxy;
	MarkDirty(Proxy);
	MarkRenderStateDirty(Proxy);

	PrimitiveSceneProxies.Add(Proxy);
	PrimitiveBounds.Add(FAABB{});
	PrimitiveFlags.Add(0);
	ProxyCachedCommands.AddDefaulted_GetRef();
	GPUScenePrimitives.AddDefaulted_GetRef();
	GPUSceneDirtyMask.Add(0);
	UpdateGPUScenePrimitiveEntry(Index, Proxy);
	if (Proxy->HasStaticRelevance())
		MarkStaticCacheDirty(Proxy);

	bElementListChanged = true;
}

void FScene::RemovePrimitive(UPrimitiveComponent* Component)
{
	if (!Component || !Component->SceneProxy) return;

	FPrimitiveSceneProxy* Proxy = Component->SceneProxy;
	if (!Proxy) return;

	const uint32 Index = static_cast<uint32>(Proxy->PackedIndex);

	// 사라진 Caster의 그림자가 캐시에 남지 않도록 마지막 위치를 기록한다.
	if (Proxy->bCastsShadow) RecordShadowInvalidation(Proxy->GetBounds());

	ClearProxyStaticCache(Index);

	if (Proxy->bPendingStaticCacheRebuild)
	{
		const int32 Found = StaticCacheDirtyProxies.Find(Proxy);
		if (Found != INDEX_NONE) StaticCacheDirtyProxies.RemoveAtSwap(Found);
		Proxy->bPendingStaticCacheRebuild = false;
	}

	PrimitiveSceneProxies.RemoveAtSwap(Index);
	PrimitiveBounds.RemoveAtSwap(Index);
	PrimitiveFlags.RemoveAtSwap(Index);
	ProxyCachedCommands.RemoveAtSwap(Index);
	GPUScenePrimitives.RemoveAtSwap(Index);
	GPUSceneDirtyMask.RemoveAtSwap(Index);

	if (Index < static_cast<uint32>(PrimitiveSceneProxies.Num()))
	{
		FPrimitiveSceneProxy* SwappedProxy = PrimitiveSceneProxies[Index];
		SwappedProxy->PackedIndex = static_cast<int32>(Index);
		SwappedProxy->SetPrimitiveId(Index);
		for (const FCachedCommandSlotRef& Ref : ProxyCachedCommands[Index])
		{
			TArray<FCachedMeshDrawEntry>& List = CachedDrawLists[static_cast<size_t>(Ref.Pass)];
			if (Ref.CommandIndex < static_cast<uint32>(List.Num()))
			{
				List[Ref.CommandIndex].PrimitiveIndex = Index;
				List[Ref.CommandIndex].Command.PrimitiveId = Index;
			}
		}
		MarkGPUSceneDirty(Index);
	}

	if (Proxy->bQueuedForUpdate)
	{
		const int32 Found = DirtyProxies.Find(Proxy);   // 프로젝트 TArray의 Find 이름에 맞게
		if (Found != INDEX_NONE) DirtyProxies.RemoveAtSwap(Found);
	}

	if (Proxy->bRenderStateQueued)
	{
		const int32 Found = RenderStateDirtyProxies.Find(Proxy);
		if (Found != INDEX_NONE) RenderStateDirtyProxies.RemoveAtSwap(Found);
	}

	Component->SceneProxy = nullptr;
	delete Proxy;

	bElementListChanged = true;
}

void FScene::RemoveAllPrimitives()
{
	// 하나씩 RemovePrimitive하면 대기열 검색(Find)이 물체 수만큼 반복되므로 통째로 비운다.
	for (FPrimitiveSceneProxy* Proxy : PrimitiveSceneProxies)
	{
		if (UPrimitiveComponent* Component = Proxy->GetComponent())
			Component->SceneProxy = nullptr;
		delete Proxy;
	}
	PrimitiveSceneProxies.Reset();
	PrimitiveBounds.Reset();
	PrimitiveFlags.Reset();
	DirtyProxies.Reset();
	RenderStateDirtyProxies.Reset();
	StaticCacheDirtyProxies.Reset();
	for (TArray<FCachedMeshDrawEntry>& List : CachedDrawLists)
		List.Reset();
	ProxyCachedCommands.Reset();
	GPUScenePrimitives.Reset();
	GPUSceneDirtyIndices.Reset();
	GPUSceneDirtyMask.Reset();
	BVH.Clear();
	bElementListChanged = true;
	InvalidateAllShadows();
}

void FScene::AddLight(ULightComponent* Component)
{
	if (!Component || Component->SceneProxy) return;

	FLightSceneProxy* Proxy = Component->CreateSceneProxy();
	if (!Proxy)
	{
		if (UAmbientLightComponent* Ambient = Cast<UAmbientLightComponent>(Component))
			if (AmbientLights.Find(Ambient) == INDEX_NONE) AmbientLights.Add(Ambient);
		return;
	}

	Proxy->Scene = this;
	Component->SceneProxy = Proxy;

	if (Component->GetLightType() == ELightType::Directional)
	{
		Proxy->PackedIndex = DirectionalLightProxies.Num();
		DirectionalLightProxies.Add(static_cast<FDirectionalLightSceneProxy*>(Proxy));
	}
	else
	{
		Proxy->PackedIndex = LocalLightProxies.Num();
		LocalLightProxies.Add(static_cast<FLocalLightSceneProxy*>(Proxy));
	}

	// PackedIndex는 RemoveAtSwap 때마다 바뀌므로, Id는 Lights의 빈 칸을 재사용해 고정한다.
	FLightSceneInfo* Info = new FLightSceneInfo();
	Info->Proxy = Proxy;
	Info->Scene = this;
	Info->Id = Lights.Find(nullptr);
	if (Info->Id == INDEX_NONE)
	{
		Info->Id = Lights.Num();
		Lights.Add(Info);
	}
	else
	{
		Lights[Info->Id] = Info;
	}
	Proxy->LightSceneInfo = Info;
}

void FScene::RemoveLight(ULightComponent* Component)
{
	if (!Component) return;

	FLightSceneProxy* Proxy = Component->SceneProxy;
	if (!Proxy)
	{
		if (UAmbientLightComponent* Ambient = Cast<UAmbientLightComponent>(Component))
		{
			const int32 Found = AmbientLights.Find(Ambient);
			if (Found != INDEX_NONE) AmbientLights.RemoveAtSwap(Found);
		}
		return;
	}

	const int32 Index = Proxy->PackedIndex;
	if (Component->GetLightType() == ELightType::Directional)
	{
		DirectionalLightProxies.RemoveAtSwap(Index);
		if (Index < DirectionalLightProxies.Num())
			DirectionalLightProxies[Index]->PackedIndex = Index;
	}
	else
	{
		LocalLightProxies.RemoveAtSwap(Index);
		if (Index < LocalLightProxies.Num())
			LocalLightProxies[Index]->PackedIndex = Index;
	}

	if (FLightSceneInfo* Info = Proxy->LightSceneInfo)
	{
		if (Info->Id >= 0 && Info->Id < Lights.Num()) Lights[Info->Id] = nullptr;
		delete Info;
	}

	Component->SceneProxy = nullptr;
	delete Proxy;
}

void FScene::RemoveAllLights()
{
	for (FLightSceneInfo* Info : Lights) delete Info;
	Lights.Reset();
	for (FLightSceneProxy* Proxy : DirectionalLightProxies) delete Proxy;
	for (FLightSceneProxy* Proxy : LocalLightProxies)       delete Proxy;
	DirectionalLightProxies.Reset();
	LocalLightProxies.Reset();
	AmbientLights.Reset();
}

void FScene::AddDecal(UDecalComponent* Component)
{
	if (Component == nullptr) return;
	if (Decals.Find(Component) != INDEX_NONE) return;

	Decals.Push(Component);
}

void FScene::RemoveDecal(UDecalComponent* Component)
{
	const int32 idx = Decals.Find(Component);
	if (idx != INDEX_NONE) Decals.RemoveAtSwap(idx);
}

void FScene::RemoveAllDecals()
{
	Decals.Reset();
}

FAmbientLightRenderData FScene::GetAmbientLight(bool bGameView) const
{
	for (const UAmbientLightComponent* Ambient : AmbientLights)
	{
		if (!Ambient || !Ambient->IsShown(bGameView)) continue;
		const FLinearColor Color(Ambient->GetLightColor());
		return { FVector4(Color.R, Color.G, Color.B, Ambient->GetIntensity()) };
	}
	return {};
}

void FScene::ClearProxyStaticCache(uint32 PrimitiveIndex)
{
	if (PrimitiveIndex >= static_cast<uint32>(ProxyCachedCommands.Num()))
		return;

	TArray<FCachedCommandSlotRef>& Refs = ProxyCachedCommands[PrimitiveIndex];
	for (int32 r = Refs.Num() - 1; r >= 0; --r)
	{
		const FCachedCommandSlotRef Ref = Refs[r];
		TArray<FCachedMeshDrawEntry>& List = CachedDrawLists[static_cast<size_t>(Ref.Pass)];
		if (Ref.CommandIndex >= static_cast<uint32>(List.Num()))
			continue;

		const uint32 LastIdx = static_cast<uint32>(List.Num() - 1);
		if (Ref.CommandIndex != LastIdx)
		{
			List[Ref.CommandIndex] = std::move(List[LastIdx]);
			const FCachedMeshDrawEntry& Moved = List[Ref.CommandIndex];
			if (Moved.PrimitiveIndex < static_cast<uint32>(ProxyCachedCommands.Num()) &&
				Moved.OwnerEntryIndex < static_cast<uint32>(ProxyCachedCommands[Moved.PrimitiveIndex].Num()))
			{
				ProxyCachedCommands[Moved.PrimitiveIndex][Moved.OwnerEntryIndex].CommandIndex = Ref.CommandIndex;
			}
		}
		List.RemoveLast();
	}
	Refs.Reset();
}

void FScene::RebuildProxyStaticCache(FPrimitiveSceneProxy* Proxy, const FSceneRenderOptions& Options)
{
	if (!Proxy || Proxy->PackedIndex < 0)
		return;

	const uint32 PrimIdx = static_cast<uint32>(Proxy->PackedIndex);
	ClearProxyStaticCache(PrimIdx);
	if (!Proxy->HasStaticRelevance() || !Proxy->IsVisible())
		return;

	FSceneStaticPDI PDI;
	Proxy->DrawStaticElements(&PDI);
	if (PDI.Batches.IsEmpty())
		return;

	const FBasePassMeshProcessor BaseProcessor;
	const FDepthPassMeshProcessor DepthProcessor;
	const FWorldNormalPassMeshProcessor WorldNormalProcessor;
	const FTranslucencyPassMeshProcessor TranslucencyProcessor;
	const FShadowDepthMeshProcessor ShadowProcessor;

	for (const FStaticMeshBatch& Batch : PDI.Batches)
	{
		AppendCachedPassCommands(EMeshPass::BasePass, BaseProcessor, Batch, Options, PrimIdx, CachedDrawLists, ProxyCachedCommands);
		AppendCachedPassCommands(EMeshPass::DepthPass, DepthProcessor, Batch, Options, PrimIdx, CachedDrawLists, ProxyCachedCommands);
		AppendCachedPassCommands(EMeshPass::WorldNormal, WorldNormalProcessor, Batch, Options, PrimIdx, CachedDrawLists, ProxyCachedCommands);
		AppendCachedPassCommands(EMeshPass::Translucency, TranslucencyProcessor, Batch, Options, PrimIdx, CachedDrawLists, ProxyCachedCommands);
		AppendCachedPassCommands(EMeshPass::ShadowDepth, ShadowProcessor, Batch, Options, PrimIdx, CachedDrawLists, ProxyCachedCommands);
	}
}

void FScene::UpdateGPUScenePrimitiveEntry(uint32 PrimitiveIndex, const FPrimitiveSceneProxy* Proxy)
{
	if (!Proxy || PrimitiveIndex >= static_cast<uint32>(GPUScenePrimitives.Num()))
		return;
	const int32 DebugWeightBone = Proxy->GetDebugWeightBone();
	const float BoneOffset = DebugWeightBone >= 0 ? static_cast<float>(DebugWeightBone + 1) : 0.0f;
	GPUScenePrimitives[PrimitiveIndex] = MakePrimitiveSceneData(
		Proxy->GetLocalToWorld(), Proxy->bReceiveShadow, Proxy->bReceivesDecals, BoneOffset);
	MarkGPUSceneDirty(PrimitiveIndex);
}

void FScene::FlushStaticCaches(const FSceneRenderOptions& Options)
{
	const uint64 MaterialRevision = UMaterial::GetRenderDataRevision();
	const bool bOptionsChanged = !bHasCachedOptions || CachedLightingModel != Options.LightingModel;

	if (bOptionsChanged || CachedMaterialRevision != MaterialRevision)
	{
		bHasCachedOptions = true;
		CachedMaterialRevision = MaterialRevision;
		CachedLightingModel = Options.LightingModel;

		for (FPrimitiveSceneProxy* Proxy : PrimitiveSceneProxies)
		{
			if (Proxy && Proxy->HasStaticRelevance())
				MarkStaticCacheDirty(Proxy);
		}
	}

	if (StaticCacheDirtyProxies.IsEmpty())
		return;

	for (FPrimitiveSceneProxy* Proxy : StaticCacheDirtyProxies)
	{
		if (!Proxy)
			continue;
		Proxy->bPendingStaticCacheRebuild = false;
		RebuildProxyStaticCache(Proxy, Options);
	}
	StaticCacheDirtyProxies.Reset();
}

const FStructuredBuffer* FScene::SyncGPUScene(std::span<const FPrimitiveSceneData> DynamicPrimitives)
{
	const uint32 StaticCount = static_cast<uint32>(GPUScenePrimitives.Num());
	const uint32 DynamicCount = static_cast<uint32>(DynamicPrimitives.size());
	const uint32 TotalNeeded = StaticCount + DynamicCount;
	if (TotalNeeded == 0)
		return nullptr;

	constexpr uint32 ElemStride = sizeof(FPrimitiveSceneData);
	if (!GPUSceneBuffer || GPUSceneBufferCapacity < TotalNeeded)
	{
		uint32 NewCapacity = std::max(GPUSceneBufferCapacity * 2u, 256u);
		while (NewCapacity < TotalNeeded)
			NewCapacity *= 2u;
		GPUSceneBuffer = RenderCommand::CreateDefaultStructuredBuffer(NewCapacity, ElemStride);
		GPUSceneBufferCapacity = GPUSceneBuffer ? NewCapacity : 0;
		if (!GPUSceneBuffer)
			return nullptr;
		if (StaticCount > 0)
		{
			RenderCommand::UpdateStructuredBufferRange(GPUSceneBuffer.get(), 0, StaticCount, ElemStride, GPUScenePrimitives.GetData());
		}
		GPUSceneDirtyIndices.Reset();
		std::fill(GPUSceneDirtyMask.begin(), GPUSceneDirtyMask.end(), uint8(0));
	}
	else if (!GPUSceneDirtyIndices.IsEmpty())
	{
		// 대량 변경 시 전체를 한 번에 업로드하고 소량이면 연속 구간만 부분 갱신한다
		const uint32 DirtyCount = static_cast<uint32>(GPUSceneDirtyIndices.Num());
		if (StaticCount > 0 && DirtyCount * 3 >= StaticCount)
		{
			RenderCommand::UpdateStructuredBufferRange(GPUSceneBuffer.get(), 0, StaticCount, ElemStride, GPUScenePrimitives.GetData());
		}
		else
		{
			std::sort(GPUSceneDirtyIndices.begin(), GPUSceneDirtyIndices.end());
			const uint32 NumDirty = static_cast<uint32>(GPUSceneDirtyIndices.Num());
			uint32 i = 0;
			while (i < NumDirty)
			{
				const uint32 RunStart = GPUSceneDirtyIndices[i];
				if (RunStart >= StaticCount)
					break;
				uint32 RunEnd = RunStart;
				while (i + 1 < NumDirty && GPUSceneDirtyIndices[i + 1] <= RunEnd + 1 && GPUSceneDirtyIndices[i + 1] < StaticCount)
				{
					++i;
					RunEnd = GPUSceneDirtyIndices[i];
				}
				const uint32 RunCount = RunEnd - RunStart + 1;
				RenderCommand::UpdateStructuredBufferRange(GPUSceneBuffer.get(), RunStart, RunCount, ElemStride,
					GPUScenePrimitives.GetData() + RunStart);
				++i;
			}
		}
		GPUSceneDirtyIndices.Reset();
		std::fill(GPUSceneDirtyMask.begin(), GPUSceneDirtyMask.end(), uint8(0));
	}

	if (DynamicCount > 0)
	{
		RenderCommand::UpdateStructuredBufferRange(GPUSceneBuffer.get(), StaticCount, DynamicCount, ElemStride, DynamicPrimitives.data());
	}
	return GPUSceneBuffer.get();
}

void FScene::UpdateAllTransforms()
{
	const bool bAnyRenderStateChanged = RenderStateDirtyProxies.Num() > 0;
	for (FPrimitiveSceneProxy* Proxy : RenderStateDirtyProxies)
	{
		// CastShadow·표시 여부·메시가 바뀌면 그림자 결과가 달라질 수 있다. 드물게 일어나므로 Caster 여부와 무관하게 기록한다.
		RecordShadowInvalidation(Proxy->GetBounds());
		Proxy->UpdateRenderState();
		PrimitiveFlags[Proxy->PackedIndex] = Proxy->IsVisible() ? 1 : 0;
		const uint32 Index = static_cast<uint32>(Proxy->PackedIndex);
		UpdateGPUScenePrimitiveEntry(Index, Proxy);
		if (Proxy->HasStaticRelevance())
			MarkStaticCacheDirty(Proxy);
		Proxy->bRenderStateQueued = false;
	}
	RenderStateDirtyProxies.Reset();

	for (FPrimitiveSceneProxy* Proxy : DirtyProxies)
	{
		const FAABB OldBounds = Proxy->GetBounds();
		Proxy->UpdateTransform();
		// 그림자가 사라진 자리(이전)와 새로 생긴 자리(이후)를 모두 지워야 한다.
		if (Proxy->bCastsShadow)
		{
			RecordShadowInvalidation(OldBounds);
			RecordShadowInvalidation(Proxy->GetBounds());
		}
		PrimitiveBounds[Proxy->PackedIndex] = Proxy->GetBounds();
		PrimitiveFlags[Proxy->PackedIndex] = Proxy->IsVisible() ? 1 : 0;
		UpdateGPUScenePrimitiveEntry(static_cast<uint32>(Proxy->PackedIndex), Proxy);
		Proxy->bQueuedForUpdate = false;
	}

	//const int32 Count = Proxies.Num();
	//for (int32 i = 0; i < Count; ++i)
	//{
	//	FPrimitiveSceneProxy* Proxy = Proxies[i];
	//	Proxy->UpdateTransform();

	//	const FAABB& Bounds = Proxy->GetBounds();
	//	if (PrimitiveBounds[i].Center != Bounds.Center || PrimitiveBounds[i].Extent != Bounds.Extent)
	//	{
	//		bBoundsChanged = true;
	//	}

	//	PrimitiveBounds[i] = Proxy->GetBounds();
	//	PrimitiveFlags[i] = Proxy->GetComponent()->IsVisible() ? 1 : 0;
	//}

	const bool bAnyMoved = DirtyProxies.Num() > 0 || bAnyRenderStateChanged;
	DirtyProxies.Reset();


	if (bElementListChanged)
	{
		BuildBVH();
		bElementListChanged = false;
	}
	else if (bAnyMoved)
	{
		BVH.Refit();
	}
}

void FScene::BuildBVH()
{
	BVH.Clear();
	// BillboardComponents.Reset();

	TArray<FPrimitiveSceneProxy*> Elements;
	Elements.Reserve(PrimitiveSceneProxies.Num());
	for (FPrimitiveSceneProxy* Proxy : PrimitiveSceneProxies)
	{
		if (Proxy && Proxy->GetComponent())
			Elements.Add(Proxy);
	}

	BVH.Build(std::span(Elements.GetData(), Elements.Num()));
}

void FScene::RecordShadowInvalidation(const FAABB& Bounds)
{
	if (ShadowInvalidations.Num() >= MaxShadowInvalidations)
	{
		// 오래된 절반을 버린다. 버린 기록을 아직 처리하지 않은 뷰는 OldestVersion을 보고 전부 다시 그린다.
		const int32 DropCount = ShadowInvalidations.Num() / 2;
		ShadowOldestVersion = ShadowInvalidations[DropCount - 1].Version;
		ShadowInvalidations.RemoveAt(0, DropCount);
	}
	ShadowInvalidations.Add({ ++ShadowVersion, Bounds });
}

void FScene::InvalidateAllShadows()
{
	ShadowInvalidations.Reset();
	ShadowOldestVersion = ++ShadowVersion;
}

void FScene::MarkDirty(FPrimitiveSceneProxy* Proxy)
{
	if (Proxy->bQueuedForUpdate) return;            // 중복 추가 방지
	Proxy->bQueuedForUpdate = true;
	DirtyProxies.Add(Proxy);
}

void FScene::MarkRenderStateDirty(FPrimitiveSceneProxy* Proxy)
{
	if (!Proxy || Proxy->bRenderStateQueued) return;
	Proxy->bRenderStateQueued = true;
	RenderStateDirtyProxies.Add(Proxy);
}

void FScene::MarkStaticCacheDirty(FPrimitiveSceneProxy* Proxy)
{
	if (!Proxy || Proxy->bPendingStaticCacheRebuild) return;
	Proxy->bPendingStaticCacheRebuild = true;
	StaticCacheDirtyProxies.Add(Proxy);
}

void FScene::MarkGPUSceneDirty(uint32 PrimitiveIndex)
{
	if (PrimitiveIndex >= static_cast<uint32>(GPUSceneDirtyMask.Num()))
		return;
	if (GPUSceneDirtyMask[PrimitiveIndex] != 0)
		return;
	GPUSceneDirtyMask[PrimitiveIndex] = 1;
	GPUSceneDirtyIndices.Add(PrimitiveIndex);
}

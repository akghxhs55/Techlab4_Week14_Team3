#include "EnginePCH.h"
#include "Render/MeshPassProcessor.h"
#include "Render/SceneRenderData.h"
#include "Render/RenderResourceManager.h"
#include "Render/MaterialRenderProxy.h"
#include "Render/Mesh.h"
#include "Engine/PrimitiveSceneProxy.h"
#include "Core/EngineTimer.h"
#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>

namespace
{
	inline void HashCombine(uint32& Seed, uint64 Value)
	{
		uint64 Mixed = Value * 0x9E3779B97F4A7C15ull;
		Mixed ^= Mixed >> 30;
		Seed ^= static_cast<uint32>(Mixed) + 0x9E3779B9u + (Seed << 6) + (Seed >> 2);
		Seed ^= static_cast<uint32>(Mixed >> 32) + 0x9E3779B9u + (Seed << 6) + (Seed >> 2);
	}

	inline void HashBytes(uint32& Seed, const void* Data, uint32 Size)
	{
		const uint8* Bytes = static_cast<const uint8*>(Data);
		for (uint32 i = 0; i < Size; ++i)
		{
			Seed ^= Bytes[i];
			Seed *= 16777619u;
		}
	}

	float GetNormalEncoding(UTexture2D* Texture)
	{
		if (!Texture || !Texture->GetResource() || !Texture->GetResource()->GetSRV())
			return 0.0f;
		switch (Texture->GetResource()->GetFormat())
		{
		case DXGI_FORMAT_BC5_UNORM: return 2.0f;
		case DXGI_FORMAT_BC5_SNORM: return 3.0f;
		default: return 1.0f;
		}
	}

	const FMatrix& ResolveElementWorld(const FMeshBatchElement& Element)
	{
		if (Element.WorldMatrix)
			return *Element.WorldMatrix;
		return Element.Proxy ? Element.Proxy->GetLocalToWorld() : FMatrix::Identity;
	}

	uint32 ResolveElementPrimitiveId(const FMeshBatchElement& Element)
	{
		if (Element.PrimitiveId != InvalidPrimitiveId)
			return Element.PrimitiveId;
		if (Element.Proxy)
			return Element.Proxy->GetPrimitiveId();
		return InvalidPrimitiveId;
	}

	FSceneRenderOptions ResolveMaterialLighting(const FMaterialRenderProxy& Material, const FSceneRenderOptions& Options)
	{
		FSceneRenderOptions Effective = Options;
		if (Material.bOverrideShadingModel && Options.LightingModel != ELightingModel::Unlit)
		{
			Effective.LightingModel = Material.ShadingModel;
		}
		return Effective;
	}

	EMaterialUsage ResolveMaterialGraphUsage(EPSOType PSO, EVertexFactoryType VFType)
	{
		if (VFType == EVertexFactoryType::Trail)
		{
			return EMaterialUsage::ParticleBeamTrail;
		}
		if (VFType == EVertexFactoryType::MeshParticle || VFType == EVertexFactoryType::GpuMeshParticle)
		{
			return EMaterialUsage::ParticleMesh;
		}
		if (VFType == EVertexFactoryType::GpuSpriteParticle)
		{
			return EMaterialUsage::ParticleSprite;
		}
		switch (PSO)
		{
		case EPSOType::Particle_AlphaBlend:
		case EPSOType::Particle_Additive:
			return EMaterialUsage::ParticleSprite;
		case EPSOType::StaticMesh_Forward:
		case EPSOType::StaticMesh_Translucent:
		case EPSOType::StaticMesh_Additive:
			return VFType == EVertexFactoryType::GpuSkin ? EMaterialUsage::SkeletalMesh : EMaterialUsage::StaticMesh;
		default:
			return EMaterialUsage::None;
		}
	}

	FShaderProgram* FindGraphShader(const FMaterialRenderProxy& Material, EPSOType PSO,
		ELightingModel LightingModel, EVertexFactoryType VFType)
	{
		const UMaterial* Base = Material.BaseMaterial;
		if (!Base || !Base->HasGraphOutput())
		{
			return nullptr;
		}

		const EMaterialUsage RequiredUsage = ResolveMaterialGraphUsage(PSO, VFType);
		if (RequiredUsage == EMaterialUsage::None || !Base->HasUsage(RequiredUsage))
		{
			return nullptr;
		}

		FString Error;
		return FRenderResourceManager::GetMaterialGraphShaderProgram(*Base, PSO, LightingModel, VFType, Error);
	}

	void AppendGraphBindings(const FMaterialRenderProxy& Material, TArray<FMeshDrawShaderResource>& Resources,
		TArray<FMeshDrawConstantBuffer>& ConstantBuffers)
	{
		if (!Material.BaseMaterial)
		{
			return;
		}

		for (int32 Index = 0; Index < Material.GraphTextures.Num(); ++Index)
		{
			Resources.Add({ 2u + static_cast<uint32>(Index), EShaderBindFlagBits::Pixel, Material.GraphTextures[Index], nullptr });
		}

		FMeshDrawConstantBuffer& CB = ConstantBuffers.AddDefaulted_GetRef();
		CB.Slot = 4;
		CB.Stages = EShaderBindFlagBits::Pixel;

		const uint32 BufferSize = sizeof(FVector4) * MaxMaterialGraphParameters;
		static_assert(BufferSize <= sizeof(CB.Data));

		TArray<FMaterialGraphParameter> GraphParameters;
		Material.BaseMaterial->GetGraphParameterLayout(GraphParameters);

		for (int32 ParameterIndex = 0; ParameterIndex < GraphParameters.Num() && ParameterIndex < MaxMaterialGraphParameters; ++ParameterIndex)
		{
			const FMaterialGraphParameter& Parameter = GraphParameters[ParameterIndex];
			FVector4 Value = Parameter.Type == EMaterialExpressionType::ScalarParameter
				? FVector4(Parameter.ScalarValue, 0.0f, 0.0f, 0.0f)
				: Parameter.VectorValue;

			const FName Name(Parameter.Name);
			const EMaterialParameterType OverrideType = Parameter.Type == EMaterialExpressionType::ScalarParameter
				? EMaterialParameterType::Scalar
				: EMaterialParameterType::Vector;

			for (const FMaterialParameterOverride& Override : Material.ParameterOverrides)
			{
				if (Override.ParameterName != Name || Override.Type != OverrideType)
				{
					continue;
				}
				if (OverrideType == EMaterialParameterType::Scalar)
				{
					Value.X = Override.ScalarValue;
				}
				else
				{
					Value = Override.VectorValue;
				}
				break;
			}

			std::memcpy(CB.Data.data() + ParameterIndex * sizeof(FVector4), &Value, sizeof(FVector4));
		}

		CB.DataSize = BufferSize;
	}

	FForwardMaterialConstants MakeForwardMaterial(const FMaterialRenderProxy& Material)
	{
		FForwardMaterialConstants Params{};
		Params.BaseColor = Material.BaseColor;
		const FVector ScaledEmissive = Material.EmissiveColor * Material.EmissiveFactor;
		Params.Emissive = FVector4(ScaledEmissive.X, ScaledEmissive.Y, ScaledEmissive.Z, 0.0f);
		const FVector ScaledSpecular = Material.SpecularColor * Material.SpecularFactor;
		Params.SpecularColorShininess = FVector4(
			ScaledSpecular.X,
			ScaledSpecular.Y,
			ScaledSpecular.Z,
			std::max(1.0f, Material.Shininess));
		UTexture2D* Color = Material.Textures.IsEmpty() ? nullptr : Material.Textures[0];
		UTexture2D* Normal = Material.Textures.Num() > 1 ? Material.Textures[1] : nullptr;
		const bool bHasColor = Color && Color->GetResource() && Color->GetResource()->GetSRV();
		Params.SurfaceParams = FVector4(
			Material.bFlipNormalY ? 1.0f : 0.0f,
			Material.bAlphaMasked ? 1.0f : 0.0f,
			bHasColor ? 1.0f : 0.0f,
			GetNormalEncoding(Normal));
		Params.UVTransform = FVector4(0.0f, 0.0f, Material.UVTiling.X, Material.UVTiling.Y);
		const bool bWorldUV = Material.WorldUVTileSize.X > 0.0f;
		Params.WorldUVParams = FVector4(
			bWorldUV ? 1.0f : 0.0f,
			Material.WorldUVTileSize.Y > 0.0f ? Material.WorldUVTileSize.Y : Material.WorldUVTileSize.X,
			Material.bWorldUVLocalSpace ? 1.0f : 0.0f,
			Material.BlendState == EBlendState::Opaque ? 1.0f : 0.0f);
		Params.UVScrollAndFlags = FVector4(Material.UVScrollSpeed.X, Material.UVScrollSpeed.Y, 0.0f, 0.0f);
		return Params;
	}

	FStaticMeshMaterialParams MakeStaticMaterial(const FMaterialRenderProxy& Material)
	{
		FStaticMeshMaterialParams Params{};
		Params.BaseColor = Material.BaseColor;
		Params.UVOffset = Material.UVScrollSpeed * EngineTimer::GetTotalTime();
		Params.TextureFlags.X = GetNormalEncoding(Material.Textures.Num() > 1 ? Material.Textures[1] : nullptr);
		Params.TextureFlags.Y = Material.bFlipNormalY ? 1.0f : 0.0f;
		Params.UVTiling = Material.UVTiling;
		Params.WorldUVTileSize = Material.WorldUVTileSize;
		const FVector ScaledEmissive = Material.EmissiveColor * Material.EmissiveFactor;
		Params.Emissive = FVector4(ScaledEmissive.X, ScaledEmissive.Y, ScaledEmissive.Z, 0.0f);
		const FVector ScaledSpecular = Material.SpecularColor * Material.SpecularFactor;
		Params.SpecularColorShininess = FVector4(ScaledSpecular.X, ScaledSpecular.Y, ScaledSpecular.Z, std::max(1.0f, Material.Shininess));
		Params.WorldUVParams = FVector4(Material.bWorldUVLocalSpace ? 1.0f : 0.0f, Material.BlendState == EBlendState::Opaque ? 1.0f : 0.0f, 0.0f, 0.0f);
		return Params;
	}

	void AppendMaterialTextures(const FMaterialRenderProxy& Material, bool bUsesUberShader, TArray<FMeshDrawShaderResource>& OutResources, bool bSkipDefaultColorNormal = false)
	{
		if (bSkipDefaultColorNormal)
		{
			return;
		}
		const int32 TextureCount = bUsesUberShader ? std::min(Material.Textures.Num(), 2) : Material.Textures.Num();
		OutResources.Reserve(OutResources.Num() + TextureCount);
		for (int32 Index = 0; Index < TextureCount; ++Index)
		{
			UTexture2D* Texture = Material.Textures[Index];
			EShaderBindFlagBits Stages = EShaderBindFlagBits::Pixel;
			if (bUsesUberShader && Index == 1)
				Stages = EShaderBindFlagBits::Vertex | EShaderBindFlagBits::Pixel;
			if (bUsesUberShader && Index == 0 && (!Texture || !Texture->GetResource() || !Texture->GetResource()->GetSRV()))
				Texture = nullptr;
			if (bUsesUberShader && Index == 1 && GetNormalEncoding(Texture) <= 0.5f)
				Texture = nullptr;
			OutResources.Add({ static_cast<uint32>(Index), Stages, Texture, nullptr });
		}
	}

	FMeshDrawConstantBuffer MakeForwardMaterialCB(const FMaterialRenderProxy& Material)
	{
		FMeshDrawConstantBuffer CB{};
		CB.Slot = 2;
		CB.Stages = EShaderBindFlagBits::Vertex | EShaderBindFlagBits::Pixel;
		const FForwardMaterialConstants Params = MakeForwardMaterial(Material);
		static_assert(sizeof(Params) <= sizeof(CB.Data));
		std::memcpy(CB.Data.data(), &Params, sizeof(Params));
		CB.DataSize = sizeof(Params);
		return CB;
	}

	void FinalizeCommandKeys(FMeshDrawCommand& Cmd, uint16 MaterialSortID, uint16 MeshSortID, uint8 LODIndex, uint64 OverrideSortKey = 0)
	{
		Cmd.StateBucketId = ComputeMeshCommandStateBucket(Cmd);
		Cmd.SortKey = OverrideSortKey != 0 ? OverrideSortKey
			: MakeOpaqueSortKey(Cmd.StateBucketId, MaterialSortID, MeshSortID, LODIndex);
	}

	bool BuildForwardUberCommands(const FMeshBatch& Batch, const FSceneRenderOptions& Options,
		bool bWorldNormal, TArray<FMeshDrawCommand>& OutCommands)
	{
		const FVertexFactory* VF = Batch.ResolveVertexFactory();
		if (!VF || !VF->IsValid() || !Batch.Material)
			return false;

		FMaterialRenderProxy Material{};
		Batch.Material->GetRenderProxy(Material);
		const FSceneRenderOptions Effective = ResolveMaterialLighting(Material, Options);

		FPipelineState* Base = FRenderResourceManager::GetPSO(bWorldNormal
			? EPSOType::StaticMesh_WorldNormal : EPSOType::StaticMesh_Forward);
		FShaderProgram* GraphShader = bWorldNormal
			? nullptr
			: FindGraphShader(Material, EPSOType::StaticMesh_Forward, Effective.LightingModel, VF->GetType());
		FShaderProgram* BaseShader = bWorldNormal ? (Base ? Base->Shader : nullptr)
			: FRenderResourceManager::GetForwardShaderProgram(Effective.LightingModel);
		FShaderProgram* Shader = GraphShader ? GraphShader : FRenderResourceManager::ResolveShader(BaseShader, VF->GetType());
		if (!Base || !Shader || !Shader->VertexShader || !Shader->PixelShader)
			return false;

		FShaderProgram* UnlitShader = nullptr;
		if (!bWorldNormal)
		{
			UnlitShader = GraphShader
				? (Effective.LightingModel == ELightingModel::Unlit ? GraphShader : FindGraphShader(Material, EPSOType::StaticMesh_Forward, ELightingModel::Unlit, VF->GetType()))
				: FRenderResourceManager::ResolveShader(FRenderResourceManager::GetForwardShaderProgram(ELightingModel::Unlit), VF->GetType());
		}

		const FMeshDrawConstantBuffer MaterialCB = MakeForwardMaterialCB(Material);
		TArray<FMeshDrawShaderResource> BaseResources;
		AppendMaterialTextures(Material, true, BaseResources, GraphShader != nullptr);

		const bool bIsGpuParticle = VF->GetType() == EVertexFactoryType::GpuSpriteParticle ||
			VF->GetType() == EVertexFactoryType::GpuMeshParticle;
		FMeshDrawConstantBuffer GpuDrawCB{};
		FMeshDrawConstantBuffer GpuParticleMatCB{};
		if (bIsGpuParticle)
		{
			GpuDrawCB.Slot = 1;
			GpuDrawCB.Stages = EShaderBindFlagBits::Vertex;
			if (Batch.MaterialParamData && Batch.MaterialParamDataSize > 0 && Batch.MaterialParamDataSize <= GpuDrawCB.Data.size())
			{
				std::memcpy(GpuDrawCB.Data.data(), Batch.MaterialParamData, Batch.MaterialParamDataSize);
				GpuDrawCB.DataSize = Batch.MaterialParamDataSize;
			}
			FGpuParticleMaterialConstants ParticleMat{};
			ParticleMat.MaterialBaseColor = Material.BaseColor;
			ParticleMat.HasTexture = (!Material.Textures.IsEmpty() && Material.Textures[0] && Material.Textures[0]->GetResource()) ? 1.0f : 0.0f;
			GpuParticleMatCB.Slot = 2;
			GpuParticleMatCB.Stages = EShaderBindFlagBits::Pixel;
			static_assert(sizeof(ParticleMat) <= sizeof(GpuParticleMatCB.Data));
			std::memcpy(GpuParticleMatCB.Data.data(), &ParticleMat, sizeof(ParticleMat));
			GpuParticleMatCB.DataSize = sizeof(ParticleMat);
		}

		const bool bSkeletal = VF->GetType() == EVertexFactoryType::GpuSkin;
		const bool bIsMeshParticle = VF->GetType() == EVertexFactoryType::MeshParticle || bIsGpuParticle;

		for (const FMeshBatchElement& Element : Batch.Elements)
		{
			FIndexBuffer* IB = Element.IndexBuffer ? Element.IndexBuffer : VF->GetIndexBuffer();
			if (!IB)
				continue;

			FMeshDrawCommand Cmd{};
			Cmd.Pipeline = *Base;
			if (Material.bAlphaMasked)
			{
				// 알파 마스크(나무 카드·철망 등)는 단면 폴리곤이 많으므로 양면 렌더링한다.
				Cmd.Pipeline.RasterizerState = ERasterizerState::SolidNone;
			}
			Cmd.Pipeline.Shader = Shader;
			Cmd.VertexFactory = VF;
			Cmd.FirstIndex = Element.FirstIndex;
			Cmd.IndexCount = Element.IndexCount ? Element.IndexCount : IB->GetIndexCount();
			Cmd.BaseVertexLocation = Element.BaseVertexLocation;
			Cmd.InstanceCount = Element.InstanceCount;
			Cmd.StartInstanceLocation = Element.StartInstance;
			Cmd.Sampler = Material.SamplerState;
			Cmd.SamplerStages = EShaderBindFlagBits::Vertex | EShaderBindFlagBits::Pixel;
			Cmd.ShaderResources = BaseResources;
			if (bIsGpuParticle)
			{
				if (GpuDrawCB.DataSize > 0)
				{
					Cmd.ConstantBuffers.Add(GpuDrawCB);
				}
				Cmd.ConstantBuffers.Add(GpuParticleMatCB);
			}
			else
			{
				Cmd.ConstantBuffers.Add(MaterialCB);
			}
			if (GraphShader)
			{
				AppendGraphBindings(Material, Cmd.ShaderResources, Cmd.ConstantBuffers);
			}
			Cmd.UnlitShaderOverride = UnlitShader;
			Cmd.PrimitiveId = ResolveElementPrimitiveId(Element);
			Cmd.Material = Batch.Material;
			Cmd.bUsesGPUScene = Batch.bUsesGPUScene && (Element.InstanceCount == 0) && !bIsMeshParticle;
			Cmd.bSkeletalMesh = bSkeletal;
			Cmd.bIsParticle = Batch.bIsParticle;
			Cmd.bReceiveDecal = Element.bReceiveDecal;
			Cmd.DebugWeightBone = Element.DebugWeightBone;
			Cmd.ObjectKey = Element.Proxy ? static_cast<const void*>(Element.Proxy) : static_cast<const void*>(&Element);
			Cmd.Proxy = Element.Proxy;

			const uint64 OverrideKey = Batch.bIsParticle ? (1ull << 63) : 0ull;
			FinalizeCommandKeys(Cmd, Batch.Material->GetSortID(), Batch.MeshSortID, Batch.LODIndex, OverrideKey);
			OutCommands.Add(std::move(Cmd));
		}
		return true;
	}

	bool AreShaderResourcesEqual(const TArray<FMeshDrawShaderResource>& A, const TArray<FMeshDrawShaderResource>& B)
	{
		if (A.Num() != B.Num())
			return false;
		for (int32 i = 0; i < A.Num(); ++i)
		{
			if (A[i].Slot != B[i].Slot || A[i].Stages != B[i].Stages ||
				A[i].Texture != B[i].Texture || A[i].StructuredBuffer != B[i].StructuredBuffer)
				return false;
		}
		return true;
	}

	bool AreConstantBuffersEqual(const TArray<FMeshDrawConstantBuffer>& A, const TArray<FMeshDrawConstantBuffer>& B)
	{
		if (A.Num() != B.Num())
			return false;
		for (int32 i = 0; i < A.Num(); ++i)
		{
			if (A[i].Slot != B[i].Slot || A[i].Stages != B[i].Stages || A[i].DataSize != B[i].DataSize)
				return false;
			if (A[i].DataSize > 0 && std::memcmp(A[i].Data.data(), B[i].Data.data(), A[i].DataSize) != 0)
				return false;
		}
		return true;
	}
}

uint32 ComputeMeshCommandStateBucket(const FMeshDrawCommand& Command)
{
	uint32 Seed = 2166136261u;
	HashCombine(Seed, reinterpret_cast<uintptr_t>(Command.Pipeline.Shader));
	HashCombine(Seed, static_cast<uint64>(Command.Pipeline.Topology));
	HashCombine(Seed, static_cast<uint64>(Command.Pipeline.RasterizerState));
	HashCombine(Seed, static_cast<uint64>(Command.Pipeline.BlendState));
	HashCombine(Seed, static_cast<uint64>(Command.Pipeline.DepthStencilState));
	HashCombine(Seed, static_cast<uint64>(Command.Pipeline.StencilRef));
	HashCombine(Seed, reinterpret_cast<uintptr_t>(Command.VertexFactory));
	HashCombine(Seed, static_cast<uint64>(Command.FirstIndex));
	HashCombine(Seed, static_cast<uint64>(Command.IndexCount));
	HashCombine(Seed, static_cast<uint64>(Command.BaseVertexLocation));
	HashCombine(Seed, static_cast<uint64>(Command.Sampler));
	HashCombine(Seed, static_cast<uint64>(Command.SamplerStages));
	HashCombine(Seed, static_cast<uint64>(Command.bUsesGPUScene ? 1 : 0));
	HashCombine(Seed, static_cast<uint64>(Command.bSkeletalMesh ? 1 : 0));
	HashCombine(Seed, static_cast<uint64>(Command.bIsParticle ? 1 : 0));

	for (const FMeshDrawShaderResource& Res : Command.ShaderResources)
	{
		HashCombine(Seed, Res.Slot);
		HashCombine(Seed, static_cast<uint64>(Res.Stages));
		HashCombine(Seed, reinterpret_cast<uintptr_t>(Res.Texture));
		HashCombine(Seed, reinterpret_cast<uintptr_t>(Res.StructuredBuffer));
	}
	for (const FMeshDrawConstantBuffer& CB : Command.ConstantBuffers)
	{
		HashCombine(Seed, CB.Slot);
		HashCombine(Seed, static_cast<uint64>(CB.Stages));
		HashCombine(Seed, CB.DataSize);
		if (CB.DataSize > 0)
			HashBytes(Seed, CB.Data.data(), CB.DataSize);
	}
	return Seed;
}

uint64 MakeOpaqueSortKey(uint32 StateBucketId, uint16 MaterialSortID, uint16 MeshSortID, uint8 LODIndex)
{
	return (static_cast<uint64>(StateBucketId) << 32)
		| (static_cast<uint64>(MaterialSortID & 0x3FFFu) << 18)
		| (static_cast<uint64>(MeshSortID & 0xFFFFu) << 2)
		| static_cast<uint64>(LODIndex & 0x3u);
}

uint64 MakeTranslucentSortKey(float CameraDistanceSq)
{
	const uint64 DistanceBits = static_cast<uint64>(std::bit_cast<uint32>(CameraDistanceSq));
	return (1ull << 63) | (~DistanceBits & 0xFFFFFFFFull);
}

bool AreVisibleCommandsInstanceCompatible(const FVisibleMeshDrawCommand& A, const FVisibleMeshDrawCommand& B)
{
	if (!A.Command || !B.Command)
		return false;
	const FMeshDrawCommand& CmdA = *A.Command;
	const FMeshDrawCommand& CmdB = *B.Command;
	if (!CmdA.bUsesGPUScene || !CmdB.bUsesGPUScene)
		return false;
	if (CmdA.InstanceCount != 0 || CmdB.InstanceCount != 0)
		return false;
	if (A.StateBucketId != B.StateBucketId || A.ShaderOverride != B.ShaderOverride)
		return false;
	if (A.HeatmapWireMode != B.HeatmapWireMode)
		return false;
	if (A.bForceWireframe != B.bForceWireframe || A.bOverrideStencilRef != B.bOverrideStencilRef)
		return false;
	if (A.bOverrideStencilRef && A.StencilRef != B.StencilRef)
		return false;
	if (A.bHasDepthStencilOverride != B.bHasDepthStencilOverride)
		return false;
	if (A.bHasDepthStencilOverride && A.DepthStencilOverride != B.DepthStencilOverride)
		return false;
	if (A.bHasBlendStateOverride != B.bHasBlendStateOverride)
		return false;
	if (A.bHasBlendStateOverride && A.BlendStateOverride != B.BlendStateOverride)
		return false;

	return CmdA.Pipeline.Shader == CmdB.Pipeline.Shader &&
		CmdA.Pipeline.Topology == CmdB.Pipeline.Topology &&
		CmdA.Pipeline.RasterizerState == CmdB.Pipeline.RasterizerState &&
		CmdA.Pipeline.BlendState == CmdB.Pipeline.BlendState &&
		CmdA.Pipeline.DepthStencilState == CmdB.Pipeline.DepthStencilState &&
		CmdA.Pipeline.StencilRef == CmdB.Pipeline.StencilRef &&
		CmdA.VertexFactory == CmdB.VertexFactory &&
		CmdA.FirstIndex == CmdB.FirstIndex &&
		CmdA.IndexCount == CmdB.IndexCount &&
		CmdA.BaseVertexLocation == CmdB.BaseVertexLocation &&
		CmdA.Sampler == CmdB.Sampler &&
		CmdA.SamplerStages == CmdB.SamplerStages &&
		AreShaderResourcesEqual(CmdA.ShaderResources, CmdB.ShaderResources) &&
		AreConstantBuffersEqual(CmdA.ConstantBuffers, CmdB.ConstantBuffers);
}

FShaderProgram* ResolveUnlitShaderOverride(const FMeshDrawCommand& Command, const FSceneRenderOptions& Options)
{
	if (Options.LightingModel != ELightingModel::Unlit || Command.bIsParticle)
		return nullptr;
	return Command.UnlitShaderOverride;
}

bool FDepthPassMeshProcessor::AddMeshBatch(const FMeshBatch& Batch, const FSceneRenderOptions& Options,
	const FMeshElementCollector* Collector, TArray<FMeshDrawCommand>& OutCommands) const
{
	if (!Batch.IsOpaque() || Batch.bIsParticle)
		return true;
	if (Batch.Material)
	{
		if (const UMaterial* BaseMat = Batch.Material->GetBaseMaterial())
		{
			if (BaseMat->bAlphaMasked)
				return true;
		}
	}

	const FVertexFactory* VF = Batch.ResolveVertexFactory();
	if (!VF || !VF->IsValid())
		return true;

	FPipelineState* DepthPipeline = FRenderResourceManager::GetPSO(EPSOType::StaticMesh_DepthOnly);
	FShaderProgram* Shader = DepthPipeline ? FRenderResourceManager::ResolveShader(DepthPipeline->Shader, VF->GetType()) : nullptr;
	if (!DepthPipeline || !Shader || !Shader->VertexShader || !Shader->PixelShader)
		return false;

	const uint16 MaterialSort = Batch.Material ? Batch.Material->GetSortID() : 0;
	const bool bSkeletal = VF->GetType() == EVertexFactoryType::GpuSkin;

	for (const FMeshBatchElement& Element : Batch.Elements)
	{
		FIndexBuffer* IB = Element.IndexBuffer ? Element.IndexBuffer : VF->GetIndexBuffer();
		if (!IB)
			continue;

		FMeshDrawCommand Cmd{};
		Cmd.Pipeline = *DepthPipeline;
		Cmd.Pipeline.Shader = Shader;
		Cmd.VertexFactory = VF;
		Cmd.FirstIndex = Element.FirstIndex;
		Cmd.IndexCount = Element.IndexCount ? Element.IndexCount : IB->GetIndexCount();
		Cmd.BaseVertexLocation = Element.BaseVertexLocation;
		Cmd.InstanceCount = Element.InstanceCount;
		Cmd.StartInstanceLocation = Element.StartInstance;
		Cmd.PrimitiveId = ResolveElementPrimitiveId(Element);
		Cmd.Material = Batch.Material;
		Cmd.bUsesGPUScene = Batch.bUsesGPUScene && (Element.InstanceCount == 0);
		Cmd.bSkeletalMesh = bSkeletal;
		Cmd.bReceiveDecal = Element.bReceiveDecal;
		Cmd.DebugWeightBone = Element.DebugWeightBone;
		Cmd.ObjectKey = Element.Proxy ? static_cast<const void*>(Element.Proxy) : static_cast<const void*>(&Element);
		Cmd.Proxy = Element.Proxy;
		FinalizeCommandKeys(Cmd, MaterialSort, Batch.MeshSortID, Batch.LODIndex);
		OutCommands.Add(std::move(Cmd));
	}
	return true;
}

bool FBasePassMeshProcessor::AddMeshBatch(const FMeshBatch& Batch, const FSceneRenderOptions& Options,
	const FMeshElementCollector* Collector, TArray<FMeshDrawCommand>& OutCommands) const
{
	if (!Batch.IsOpaque() || !Batch.Material)
		return true;

	return BuildForwardUberCommands(Batch, Options, false, OutCommands);
}

bool FWorldNormalPassMeshProcessor::AddMeshBatch(const FMeshBatch& Batch, const FSceneRenderOptions& Options,
	const FMeshElementCollector* Collector, TArray<FMeshDrawCommand>& OutCommands) const
{
	if (!Batch.IsOpaque() || Batch.bIsParticle || !Batch.Material)
		return true;
	if (Batch.Material->GetPSOType() != EPSOType::StaticMesh_Opaque)
		return true;

	return BuildForwardUberCommands(Batch, Options, true, OutCommands);
}

bool FTranslucencyPassMeshProcessor::AddMeshBatch(const FMeshBatch& Batch, const FSceneRenderOptions& Options,
	const FMeshElementCollector* Collector, TArray<FMeshDrawCommand>& OutCommands) const
{
	if (Batch.IsOpaque() || !Batch.Material)
		return true;

	const FVertexFactory* VF = Batch.ResolveVertexFactory();
	if (!VF || !VF->IsValid())
		return true;

	FMaterialRenderProxy Material{};
	Batch.Material->GetRenderProxy(Material);

	const EPSOType PSOType = Batch.OverridePSOType != EPSOType::Count
		? Batch.OverridePSOType : Material.PSOType;
	FPipelineState* Pipeline = FRenderResourceManager::GetPSO(PSOType);
	if (!Pipeline)
		return false;

	FShaderProgram* GraphShader = FindGraphShader(Material, PSOType, Options.LightingModel, VF->GetType());
	FShaderProgram* ResolvedShader = GraphShader ? GraphShader : FRenderResourceManager::ResolveShader(Pipeline->Shader, VF->GetType());
	if (!ResolvedShader || !ResolvedShader->VertexShader || !ResolvedShader->PixelShader)
		return false;

	const bool bIsStaticMaterialPSO =
		PSOType == EPSOType::StaticMesh_Opaque ||
		PSOType == EPSOType::StaticMesh_Translucent ||
		PSOType == EPSOType::StaticMesh_Additive ||
		PSOType == EPSOType::StaticMesh_Wireframe;

	const bool bIsGpuParticle = VF->GetType() == EVertexFactoryType::GpuSpriteParticle ||
		VF->GetType() == EVertexFactoryType::GpuMeshParticle;

	const bool bUsesGPUSceneShader = bIsStaticMaterialPSO &&
		VF->GetType() != EVertexFactoryType::MeshParticle &&
		!bIsGpuParticle &&
		Batch.bUsesGPUScene;

	FMeshDrawConstantBuffer MaterialCB{};
	FMeshDrawConstantBuffer ParticleMatCB{};
	MaterialCB.Slot = 1;
	MaterialCB.Stages = EShaderBindFlagBits::Pixel;
	if (bIsGpuParticle)
	{
		MaterialCB.Stages = EShaderBindFlagBits::Vertex;
		if (Batch.MaterialParamData && Batch.MaterialParamDataSize > 0 && Batch.MaterialParamDataSize <= MaterialCB.Data.size())
		{
			std::memcpy(MaterialCB.Data.data(), Batch.MaterialParamData, Batch.MaterialParamDataSize);
			MaterialCB.DataSize = Batch.MaterialParamDataSize;
		}
		FGpuParticleMaterialConstants ParticleMat{};
		ParticleMat.MaterialBaseColor = Material.BaseColor;
		ParticleMat.HasTexture = (!Material.Textures.IsEmpty() && Material.Textures[0] && Material.Textures[0]->GetResource()) ? 1.0f : 0.0f;
		ParticleMatCB.Slot = 2;
		ParticleMatCB.Stages = EShaderBindFlagBits::Pixel;
		static_assert(sizeof(ParticleMat) <= sizeof(ParticleMatCB.Data));
		std::memcpy(ParticleMatCB.Data.data(), &ParticleMat, sizeof(ParticleMat));
		ParticleMatCB.DataSize = sizeof(ParticleMat);
	}
	else if (bIsStaticMaterialPSO)
	{
		const FStaticMeshMaterialParams Params = MakeStaticMaterial(Material);
		static_assert(sizeof(Params) <= sizeof(MaterialCB.Data));
		std::memcpy(MaterialCB.Data.data(), &Params, sizeof(Params));
		MaterialCB.DataSize = sizeof(Params);
	}
	else if (Batch.MaterialParamData && Batch.MaterialParamDataSize > 0 && Batch.MaterialParamDataSize <= MaterialCB.Data.size())
	{
		std::memcpy(MaterialCB.Data.data(), Batch.MaterialParamData, Batch.MaterialParamDataSize);
		MaterialCB.DataSize = Batch.MaterialParamDataSize;
		if (PSOType == EPSOType::Particle_Additive || PSOType == EPSOType::Particle_AlphaBlend)
			MaterialCB.Stages = EShaderBindFlagBits::Vertex | EShaderBindFlagBits::Pixel;
	}

	TArray<FMeshDrawShaderResource> Resources;
	AppendMaterialTextures(Material, false, Resources, GraphShader != nullptr);

	const uint64 SortKey = MakeTranslucentSortKey(Batch.CameraDistanceSq);
	const bool bSkeletal = VF->GetType() == EVertexFactoryType::GpuSkin;

	for (const FMeshBatchElement& Element : Batch.Elements)
	{
		FIndexBuffer* IB = Element.IndexBuffer ? Element.IndexBuffer : VF->GetIndexBuffer();
		if (!IB)
			continue;

		FMeshDrawCommand Cmd{};
		Cmd.Pipeline = *Pipeline;
		if (Batch.bTwoSided)
			Cmd.Pipeline.RasterizerState = ERasterizerState::SolidNone;
		Cmd.Pipeline.Shader = ResolvedShader;
		Cmd.VertexFactory = VF;
		Cmd.FirstIndex = Element.FirstIndex;
		Cmd.IndexCount = Element.IndexCount ? Element.IndexCount : IB->GetIndexCount();
		Cmd.BaseVertexLocation = Element.BaseVertexLocation;
		Cmd.InstanceCount = Element.InstanceCount;
		Cmd.StartInstanceLocation = Element.StartInstance;
		Cmd.Sampler = Material.SamplerState;
		Cmd.SamplerStages = EShaderBindFlagBits::Pixel;
		Cmd.ShaderResources = Resources;
		if (MaterialCB.DataSize > 0)
			Cmd.ConstantBuffers.Add(MaterialCB);
		if (bIsGpuParticle && ParticleMatCB.DataSize > 0)
			Cmd.ConstantBuffers.Add(ParticleMatCB);
		if (GraphShader)
		{
			AppendGraphBindings(Material, Cmd.ShaderResources, Cmd.ConstantBuffers);
		}

		if (bUsesGPUSceneShader && Element.InstanceCount == 0)
		{
			Cmd.PrimitiveId = ResolveElementPrimitiveId(Element);
			Cmd.bUsesGPUScene = true;
		}
		else if (PSOType == EPSOType::Billboard)
		{
			FMeshDrawConstantBuffer ObjectCB{};
			ObjectCB.Slot = 2;
			ObjectCB.Stages = EShaderBindFlagBits::Vertex;
			const FPerObjectConstants Constants = MakePerObjectConstants(
				ResolveElementWorld(Element), Element.bReceiveShadow);
			std::memcpy(ObjectCB.Data.data(), &Constants, sizeof(Constants));
			ObjectCB.DataSize = sizeof(Constants);
			Cmd.ConstantBuffers.Add(ObjectCB);
			Cmd.bUsesGPUScene = false;
		}
		else
		{
			Cmd.bUsesGPUScene = false;
		}

		Cmd.Material = Batch.Material;
		Cmd.bSkeletalMesh = bSkeletal;
		Cmd.bIsParticle = Batch.bIsParticle;
		Cmd.bReceiveDecal = Element.bReceiveDecal;
		Cmd.DebugWeightBone = Element.DebugWeightBone;
		Cmd.ObjectKey = Element.Proxy ? static_cast<const void*>(Element.Proxy) : static_cast<const void*>(&Element);
		Cmd.Proxy = Element.Proxy;
		FinalizeCommandKeys(Cmd, Batch.Material->GetSortID(), Batch.MeshSortID, Batch.LODIndex, SortKey);
		OutCommands.Add(std::move(Cmd));
	}
	return true;
}

bool FShadowDepthMeshProcessor::AddMeshBatch(const FMeshBatch& Batch, const FSceneRenderOptions& Options,
	const FMeshElementCollector* Collector, TArray<FMeshDrawCommand>& OutCommands) const
{
	if (!Batch.bCastShadow || !Batch.IsOpaque() || Batch.bIsParticle || Batch.LODIndex != 0)
		return true;
	if (Batch.Material)
	{
		if (const UMaterial* BaseMat = Batch.Material->GetBaseMaterial())
		{
			if (BaseMat->bAlphaMasked)
				return true;
		}
	}

	const FVertexFactory* VF = Batch.ResolveVertexFactory();
	if (!VF || !VF->IsValid())
		return true;

	FPipelineState* Pipeline = FRenderResourceManager::GetPSO(EPSOType::ShadowDepth);
	FShaderProgram* Shader = Pipeline ? FRenderResourceManager::ResolveShader(Pipeline->Shader, VF->GetType()) : nullptr;
	if (!Pipeline || !Shader || !Shader->VertexShader)
		return false;

	const uint16 MaterialSort = Batch.Material ? Batch.Material->GetSortID() : 0;
	const bool bSkeletal = VF->GetType() == EVertexFactoryType::GpuSkin;

	for (const FMeshBatchElement& Element : Batch.Elements)
	{
		FIndexBuffer* IB = Element.IndexBuffer ? Element.IndexBuffer : VF->GetIndexBuffer();
		if (!IB)
			continue;

		FMeshDrawCommand Cmd{};
		Cmd.Pipeline = *Pipeline;
		Cmd.Pipeline.Shader = Shader;
		Cmd.VertexFactory = VF;
		Cmd.FirstIndex = Element.FirstIndex;
		Cmd.IndexCount = Element.IndexCount ? Element.IndexCount : IB->GetIndexCount();
		Cmd.BaseVertexLocation = Element.BaseVertexLocation;
		Cmd.InstanceCount = Element.InstanceCount;
		Cmd.StartInstanceLocation = Element.StartInstance;
		Cmd.PrimitiveId = ResolveElementPrimitiveId(Element);
		Cmd.Material = Batch.Material;
		Cmd.bUsesGPUScene = Batch.bUsesGPUScene && (Element.InstanceCount == 0);
		Cmd.bSkeletalMesh = bSkeletal;
		Cmd.ObjectKey = Element.Proxy ? static_cast<const void*>(Element.Proxy) : static_cast<const void*>(&Element);
		Cmd.Proxy = Element.Proxy;
		FinalizeCommandKeys(Cmd, MaterialSort, Batch.MeshSortID, Batch.LODIndex);
		OutCommands.Add(std::move(Cmd));
	}
	return true;
}

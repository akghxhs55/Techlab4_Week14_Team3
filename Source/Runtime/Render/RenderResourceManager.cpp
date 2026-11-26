#include "EnginePCH.h"
#include "RenderResourceManager.h"

#include "RenderCommand.h"

#include "RenderUtil.h"
#include "Render/Material.h"
#include "Core/Misc/Paths.h"
#include <cstring>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <unordered_map>



struct FPipelineTableEntry
{
	EPSOType Type;
	const char* ShaderPath;
	EPrimitiveTopology Topology;
	ERasterizerState RasterizerState;
	EBlendState BlendState;
	EDepthStencilState DepthStencilState;
};

constexpr FPipelineTableEntry PipelineTable[] =
{
	{ EPSOType::StaticMesh_Opaque,       nullptr,                                            EPrimitiveTopology::TriangleList, ERasterizerState::SolidBack, EBlendState::Opaque,       EDepthStencilState::Default },
	{ EPSOType::StaticMesh_Translucent,  "EngineResources/Shaders/StaticMeshTranslucentShader.hlsl", EPrimitiveTopology::TriangleList, ERasterizerState::SolidBack, EBlendState::AlphaBlend,   EDepthStencilState::ReadOnly },
	{ EPSOType::StaticMesh_Wireframe,    nullptr,                                    EPrimitiveTopology::TriangleList, ERasterizerState::Wireframe, EBlendState::Opaque,       EDepthStencilState::Default },
	{ EPSOType::Reserved_5, nullptr, EPrimitiveTopology::TriangleList, ERasterizerState::SolidNone, EBlendState::Opaque, EDepthStencilState::Disabled },
	{ EPSOType::Grid,                    "EngineResources/Shaders/GridShader.hlsl",          EPrimitiveTopology::TriangleList, ERasterizerState::SolidNone, EBlendState::AlphaBlend,   EDepthStencilState::ReadOnly },
	{ EPSOType::Outline_Mask,            "EngineResources/Shaders/OutlineShader.hlsl",       EPrimitiveTopology::TriangleList, ERasterizerState::SolidNone, EBlendState::NoColorWrite, EDepthStencilState::StencilMask },
	{ EPSOType::Outline_Draw,            "EngineResources/Shaders/OutlineShader.hlsl",       EPrimitiveTopology::TriangleList, ERasterizerState::SolidNone, EBlendState::Opaque,       EDepthStencilState::StencilOutline },
	{ EPSOType::Reserved_9, nullptr, EPrimitiveTopology::TriangleList, ERasterizerState::SolidNone, EBlendState::Opaque, EDepthStencilState::Disabled },
	{ EPSOType::Reserved_10, nullptr, EPrimitiveTopology::TriangleList, ERasterizerState::SolidNone, EBlendState::Opaque, EDepthStencilState::Disabled },
	{ EPSOType::Reserved_11, nullptr, EPrimitiveTopology::TriangleList, ERasterizerState::SolidNone, EBlendState::Opaque, EDepthStencilState::Disabled },
	{ EPSOType::ToneMap,                "EngineResources/Shaders/ToneMapShader.hlsl",              EPrimitiveTopology::TriangleList, ERasterizerState::SolidNone, EBlendState::Opaque,       EDepthStencilState::Disabled },
	{ EPSOType::Reserved_13,            nullptr,                                            EPrimitiveTopology::TriangleList, ERasterizerState::SolidNone, EBlendState::Opaque,       EDepthStencilState::Default },
	{ EPSOType::Billboard,              "EngineResources/Shaders/BillboardShader.hlsl",            EPrimitiveTopology::TriangleList, ERasterizerState::SolidNone, EBlendState::AlphaBlend,   EDepthStencilState::ReadOnly },
	{ EPSOType::StaticMesh_Forward,		"EngineResources/Shaders/UberLit.hlsl",			EPrimitiveTopology::TriangleList,			ERasterizerState::SolidBack, EBlendState::Opaque, EDepthStencilState::Default },
	{ EPSOType::StaticMesh_WorldNormal, "EngineResources/Shaders/UberLit.hlsl", EPrimitiveTopology::TriangleList, ERasterizerState::SolidBack, EBlendState::Opaque, EDepthStencilState::Default },
	{ EPSOType::StaticMesh_DepthOnly, "EngineResources/Shaders/DepthPrepass.hlsl", EPrimitiveTopology::TriangleList, ERasterizerState::SolidBack, EBlendState::NoColorWrite, EDepthStencilState::Default },
	{ EPSOType::StaticMesh_LightShadow, "EngineResources/Shaders/DepthPrepass.hlsl", EPrimitiveTopology::TriangleList, ERasterizerState::LightShadow, EBlendState::NoColorWrite, EDepthStencilState::Default },
	{ EPSOType::LightCullingDebug, "EngineResources/Shaders/LightCullingDebug.hlsl", EPrimitiveTopology::TriangleList, ERasterizerState::SolidNone, EBlendState::AlphaBlend, EDepthStencilState::Disabled },
	{ EPSOType::ShadowTileClear, nullptr, EPrimitiveTopology::TriangleList, ERasterizerState::SolidNone, EBlendState::NoColorWrite, EDepthStencilState::AlwaysWrite },
	{ EPSOType::ShadowDepth, "EngineResources/Shaders/ShadowDepth.hlsl", EPrimitiveTopology::TriangleList, ERasterizerState::LightShadow, EBlendState::NoColorWrite, EDepthStencilState::Default },
	{ EPSOType::ShadowProjection, "EngineResources/Shaders/ShadowProjection.hlsl", EPrimitiveTopology::TriangleList, ERasterizerState::SolidNone, EBlendState::ShadowMaskR, EDepthStencilState::Disabled },
	{ EPSOType::Reserved_23, nullptr, EPrimitiveTopology::TriangleList, ERasterizerState::SolidNone, EBlendState::Opaque, EDepthStencilState::Disabled },
	{ EPSOType::Decal,			"EngineResources/Shaders/DecalShader.hlsl",			EPrimitiveTopology::TriangleList,		ERasterizerState::SolidBack, EBlendState::AlphaBlend, EDepthStencilState::ReadOnly },
	{ EPSOType::DecalAdditive,	"EngineResources/Shaders/DecalShader.hlsl",			EPrimitiveTopology::TriangleList,		ERasterizerState::SolidBack, EBlendState::Additive, EDepthStencilState::ReadOnly },
	{ EPSOType::Particle_AlphaBlend,     "EngineResources/Shaders/ParticleSpriteShader.hlsl", EPrimitiveTopology::TriangleList, ERasterizerState::SolidNone, EBlendState::AlphaBlend,   EDepthStencilState::ReadOnly },
	{ EPSOType::Particle_Additive,       "EngineResources/Shaders/ParticleSpriteShader.hlsl", EPrimitiveTopology::TriangleList, ERasterizerState::SolidNone, EBlendState::ParticleAdditive, EDepthStencilState::ReadOnly },
	{ EPSOType::StaticMesh_Additive,     "EngineResources/Shaders/StaticMeshTranslucentShader.hlsl", EPrimitiveTopology::TriangleList, ERasterizerState::SolidBack, EBlendState::ParticleAdditive, EDepthStencilState::ReadOnly },
	{ EPSOType::DBufferDecal_DepthReconstruction, "EngineResources/Shaders/ProjectedDecalDepth.hlsl", EPrimitiveTopology::TriangleList, ERasterizerState::SolidNone, EBlendState::AlphaBlend, EDepthStencilState::Disabled },
};

static_assert(sizeof(PipelineTable) / sizeof(PipelineTable[0]) == static_cast<size_t>(EPSOType::Count));

void FRenderResourceManager::InitPipelineStates()
{
	for (size_t i = 0; i < static_cast<size_t>(EPSOType::Count); ++i)
	{
		const auto& Entry = PipelineTable[i];
		const EPSOType Type = static_cast<EPSOType>(i);
		PipelineStateMap[Type] = MakeUnique<FPipelineState>();
		FPipelineState* State = PipelineStateMap[Type].get();
		// World Normal은 전용 매크로 변형이 성공했을 때만 연결한다.
		// 셰이더가 없는 항목은 번호만 남긴 예약 자리다.
		State->Shader = Type == EPSOType::StaticMesh_WorldNormal || !Entry.ShaderPath ? nullptr : GetShaderProgram(Entry.ShaderPath);
		State->Topology = Entry.Topology;
		State->RasterizerState = Entry.RasterizerState;
		State->BlendState = Entry.BlendState;
		State->DepthStencilState = Entry.DepthStencilState;
	}
	const char* Names[] = { "LIGHTING_MODEL_GOURAUD", "LIGHTING_MODEL_LAMBERT", "LIGHTING_MODEL_PHONG", "LIGHTING_MODEL_UNLIT", "VIEW_MODE_WORLD_NORMAL" };
	for (const char* Name : Names)
	{
		const FString Path = "EngineResources/Shaders/UberLit.hlsl";
		const FString Key = Path + "#" + Name;
		CompileVertexFactoryVariants(Path, Key, Name);
		if (std::strcmp(Name, "VIEW_MODE_WORLD_NORMAL") == 0)
			PipelineStateMap[EPSOType::StaticMesh_WorldNormal]->Shader = FindShaderProgram(Key);
	}
}

FShaderProgram* FRenderResourceManager::GetForwardShaderProgram(ELightingModel Model)
{
	const char* Name = "LIGHTING_MODEL_LAMBERT";
	switch (Model)
	{
	case ELightingModel::Gouraud: Name = "LIGHTING_MODEL_GOURAUD"; break;
	case ELightingModel::Phong: Name = "LIGHTING_MODEL_PHONG"; break;
	case ELightingModel::Unlit: Name = "LIGHTING_MODEL_UNLIT"; break;
	default: break;
	}
	const FString Key = FString("EngineResources/Shaders/UberLit.hlsl#") + Name;
	if (auto* Found = Get().ShaderProgramMap.Find(Key)) return Found->get();
	return nullptr;
}

FPipelineState* FRenderResourceManager::GetPSO(const EPSOType& InType)
{
	if (auto* Found = Get().PipelineStateMap.Find(InType))
	{
		return Found->get();
	}
	return nullptr;
}

void FRenderResourceManager::ScanShaders(const fs::path& ShaderRoot)
{
	std::error_code ErrorCode;

	// 셰이더는 엔진 폴더에 있다. 작업 디렉터리는 프로젝트 폴더이므로 엔진 폴더 기준으로 찾는다.
	const fs::path AbsoluteRoot = FPaths::EngineFile(ShaderRoot);
	if (!fs::exists(AbsoluteRoot, ErrorCode))
	{
		KRS_LOG(Error, "[Shader] Scan Root Not Found : {}", FPaths::ToUtf8(AbsoluteRoot));
		return;
	}

	for (const fs::directory_entry& Entry : fs::recursive_directory_iterator(AbsoluteRoot))
	{
		if (!Entry.is_regular_file()) continue;
		if (Entry.path().extension() != ".hlsl") continue;
		// 컴퓨트 전용 .hlsl에는 mainVS/mainPS가 없다. VS/PS로 컴파일하지 않는다.
		std::ifstream SourceFile(Entry.path(), std::ios::binary);
		if (!SourceFile) continue;
		const std::string Source(std::istreambuf_iterator<char>{ SourceFile }, std::istreambuf_iterator<char>{});
		if (Source.find("mainVS(") == std::string::npos ||
			Source.find("mainPS(") == std::string::npos) continue;

		// 키는 지금까지처럼 엔진 상대 경로로 둔다.
		FString Path = FPaths::ToUtf8(Entry.path().lexically_relative(FPaths::EngineDir()));
		// 메시를 그리는 셰이더는 VertexFactory.hlsli를 통해 정점을 받으므로 정점 타입마다 변형을 만든다
		if (Source.find("VertexFactory.hlsli") != std::string::npos)
			CompileVertexFactoryVariants(Path, Path, nullptr);
		else
			LoadOrCompileShader(Path);
	}
}

void FRenderResourceManager::Shutdown()
{
	Get().VertexShaderMap.Empty();
	Get().PixelShaderMap.Empty();
	Get().ComputeShaderMap.Empty();
	Get().ShaderProgramMap.Empty();
}

FComputeShader* FRenderResourceManager::GetComputeShader(const FString& InPath, const FString& EntryPoint)
{
	const FString Key = InPath + "#" + EntryPoint;
	if (TUniquePtr<FComputeShader>* Found = Get().ComputeShaderMap.Find(Key))
	{
		return Found->get();
	}

	FString CSOPath;
	FShaderByteCode CSCode = RenderUtil::GetOrCompile(InPath, EntryPoint.c_str(), EShaderType::Compute, CSOPath);
	if (!CSCode.IsValid())
	{
		KRS_LOG(Error, "[ComputeShader] compile failed: {} ({})", InPath, EntryPoint);
		return nullptr;
	}

	TUniquePtr<FComputeShader> CS = RenderCommand::CreateComputeShader(CSCode);
	if (!CS || !CS->IsValid())
	{
		return nullptr;
	}

	FComputeShader* Raw = CS.get();
	Get().ComputeShaderMap[Key] = std::move(CS);
	return Raw;
}

FShaderProgram* FRenderResourceManager::GetShaderProgram(const FString& InPath)
{
	if (TUniquePtr<FShaderProgram>* Found = Get().ShaderProgramMap.Find(InPath))
		return Found->get();

	KRS_LOG(Error, "[Shader] not found: {}", InPath);

	if (TUniquePtr<FShaderProgram>* Fallback =
		Get().ShaderProgramMap.Find("EngineResources/Shaders/DefaultShader.hlsl"))
		return Fallback->get();

	return nullptr;
}

FShaderProgram* FRenderResourceManager::FindShaderProgram(const FString& InPath)
{
	if (TUniquePtr<FShaderProgram>* Found = Get().ShaderProgramMap.Find(InPath))
		return Found->get();
	return nullptr;
}



void FRenderResourceManager::LoadOrCompileShader(const FString& Path)
{ 
	FString VSCSOPath;
	FString PSCSOPath;
	FShaderByteCode VSCode = RenderUtil::GetOrCompile(Path, "mainVS", EShaderType::Vertex, VSCSOPath);
	FShaderByteCode PSCode = RenderUtil::GetOrCompile(Path, "mainPS", EShaderType::Pixel, PSCSOPath);

	if (!VSCode.IsValid() || !PSCode.IsValid())
	{
		KRS_LOG(Error, "[Shader] compile failed: {}", Path);
		return;
	}

	TUniquePtr<FVertexShader> Vs = RenderCommand::CreateVertexShader(VSCode);
	TUniquePtr<FPixelShader>  Ps = RenderCommand::CreatePixelShader(PSCode);

	if (!Vs || !Vs->IsValid() || !Ps || !Ps->IsValid())
	{
		KRS_LOG(Error, "[Shader] device create failed: {}", Path);
		return;
	}

	FVertexShader* VsRaw = Vs.get();
	FPixelShader* PsRaw = Ps.get();

	VertexShaderMap[VSCSOPath] = std::move(Vs);
	PixelShaderMap[PSCSOPath] = std::move(Ps);
	ShaderProgramMap[Path] = MakeUnique<FShaderProgram>(VsRaw, PsRaw);
	ShaderProgramMap[Path]->Name = Path;

	KRS_LOG(Info, "[Shader] loaded: {}", Path);

}

FString FRenderResourceManager::MakeVFKey(const FString& BaseKey, EVertexFactoryType VF)
{
	// Local은 기존 키를 그대로 써서 PSO 테이블과 GetForwardShaderProgram이 Local 변형을 가리키게 한다
	if (VF == EVertexFactoryType::Local) return BaseKey;
	return BaseKey + "#" + GetVertexFactoryTypeInfo(VF).Name;
}

// 정점 타입마다 VS/PS를 같은 매크로로 컴파일하고, VS 입력 레이아웃은 정점 타입이 정한 것으로 만든다
void FRenderResourceManager::CompileVertexFactoryVariants(const FString& Path, const FString& BaseKey, const char* ExtraMacro)
{
	for (uint8 Index = 0; Index < static_cast<uint8>(EVertexFactoryType::Count); ++Index)
	{
		const EVertexFactoryType VF = static_cast<EVertexFactoryType>(Index);
		const FVertexFactoryTypeInfo& Info = GetVertexFactoryTypeInfo(VF);
		if (Info.Elements.empty()) continue;

		D3D_SHADER_MACRO Defines[3] = {};
		int32 Count = 0;
		if (ExtraMacro) Defines[Count++] = { ExtraMacro, "1" };
		Defines[Count++] = { Info.Define, "1" };

		// .cso 캐시 파일 이름에 매크로가 들어가므로 변형마다 따로 캐시된다
		FString VSCSOPath;
		FString PSCSOPath;
		FShaderByteCode VSCode = RenderUtil::GetOrCompile(Path, "mainVS", EShaderType::Vertex, VSCSOPath, Defines);
		FShaderByteCode PSCode = RenderUtil::GetOrCompile(Path, "mainPS", EShaderType::Pixel, PSCSOPath, Defines);
		if (!VSCode.IsValid() || !PSCode.IsValid())
		{
			KRS_LOG(Error, "[Shader] {} variant compile failed: {}", Info.Name, BaseKey);
			continue;
		}

		TUniquePtr<FVertexShader> VS = RenderCommand::CreateVertexShader(VSCode, Info.Elements);
		TUniquePtr<FPixelShader> PS = RenderCommand::CreatePixelShader(PSCode);
		if (!VS || !VS->IsValid() || !VS->GetLayout() || !PS || !PS->IsValid())
		{
			KRS_LOG(Error, "[Shader] {} variant create failed: {}", Info.Name, BaseKey);
			continue;
		}

		FVertexShader* VSRaw = VS.get();
		FPixelShader* PSRaw = PS.get();
		VertexShaderMap[VSCSOPath] = std::move(VS);
		PixelShaderMap[PSCSOPath] = std::move(PS);
		const FString Key = MakeVFKey(BaseKey, VF);
		ShaderProgramMap[Key] = MakeUnique<FShaderProgram>(VSRaw, PSRaw);
		ShaderProgramMap[Key]->Name = Key;
		ShaderProgramMap[Key]->bUsesVertexFactory = true;
	}
	KRS_LOG(Info, "[Shader] loaded: {}", BaseKey);
}

namespace
{
	const FMaterialExpressionNode* FindGraphNode(const UMaterial& Material, int32 NodeId)
	{
		for (const FMaterialExpressionNode& Node : Material.ExpressionNodes)
		{
			if (Node.Id == NodeId)
			{
				return &Node;
			}
		}
		return nullptr;
	}

	const FMaterialExpressionConnection* FindGraphInput(const UMaterial& Material, int32 NodeId, int32 Pin)
	{
		for (const FMaterialExpressionConnection& Connection : Material.ExpressionConnections)
		{
			if (Connection.InputNodeId == NodeId && Connection.InputPin == Pin)
			{
				return &Connection;
			}
		}
		return nullptr;
	}

	FString GraphNodeVariable(int32 NodeId)
	{
		return "GraphNode_" + std::to_string(NodeId);
	}

	FString HlslFloat(float Value)
	{
		std::ostringstream Stream;
		Stream << std::fixed << std::setprecision(6) << Value << "f";
		return Stream.str();
	}

	const char* GetMaterialHlslType(EMaterialValueType Type)
	{
		switch (Type)
		{
		case EMaterialValueType::Scalar: return "float";
		case EMaterialValueType::Vector2: return "float2";
		case EMaterialValueType::Vector3: return "float3";
		case EMaterialValueType::Vector4: return "float4";
		default: return nullptr;
		}
	}

	FString GetMaterialGraphParameterKey(const FString& Name, EMaterialExpressionType Type)
	{
		return Name + "#" + std::to_string(static_cast<uint8>(Type));
	}

	bool VisitGraphEvaluationOrder(const UMaterial& Material, int32 NodeId,
		std::unordered_map<int32, uint8>& VisitState, TArray<const FMaterialExpressionNode*>& OutOrder)
	{
		if (VisitState[NodeId] == 1)
		{
			return false;
		}
		if (VisitState[NodeId] == 2)
		{
			return true;
		}

		VisitState[NodeId] = 1;
		for (const FMaterialExpressionConnection& Connection : Material.ExpressionConnections)
		{
			if (Connection.InputNodeId == NodeId &&
				!VisitGraphEvaluationOrder(Material, Connection.OutputNodeId, VisitState, OutOrder))
			{
				return false;
			}
		}

		const FMaterialExpressionNode* Node = FindGraphNode(Material, NodeId);
		if (!Node)
		{
			return false;
		}
		if (Node->Type != EMaterialExpressionType::MaterialOutput)
		{
			OutOrder.Add(Node);
		}
		VisitState[NodeId] = 2;
		return true;
	}

	EMaterialValueType GetGraphInputType(const UMaterial& Material, int32 NodeId, int32 Pin)
	{
		const FMaterialExpressionConnection* Connection = FindGraphInput(Material, NodeId, Pin);
		return Connection ? Material.GetExpressionOutputType(Connection->OutputNodeId, Connection->OutputPin) : EMaterialValueType::Unknown;
	}

	FString GetGraphInputVariable(const UMaterial& Material, int32 NodeId, int32 Pin,
		EMaterialValueType TargetType = EMaterialValueType::Unknown)
	{
		const FMaterialExpressionConnection* Connection = FindGraphInput(Material, NodeId, Pin);
		if (!Connection)
		{
			return FString();
		}

		const FString Variable = GraphNodeVariable(Connection->OutputNodeId);
		const EMaterialValueType SourceType = Material.GetExpressionOutputType(Connection->OutputNodeId, Connection->OutputPin);
		if (TargetType == EMaterialValueType::Unknown || SourceType == TargetType || SourceType != EMaterialValueType::Scalar)
		{
			return Variable;
		}

		switch (TargetType)
		{
		case EMaterialValueType::Vector2: return "float2(" + Variable + ", " + Variable + ")";
		case EMaterialValueType::Vector3: return "float3(" + Variable + ", " + Variable + ", " + Variable + ")";
		case EMaterialValueType::Vector4: return "float4(" + Variable + ", " + Variable + ", " + Variable + ", " + Variable + ")";
		default: return Variable;
		}
	}

	void EmitGraphOutputPin(const UMaterial& Material, int32 Pin, const char* Define, const char* Field,
		bool bVector4, bool bVector3, FString& OutSource)
	{
		const FMaterialExpressionConnection* Connection = FindGraphInput(Material, Material.OutputNodeId, Pin);
		if (!Connection)
		{
			return;
		}

		const FString Variable = GraphNodeVariable(Connection->OutputNodeId);
		const EMaterialValueType Type = Material.GetExpressionOutputType(Connection->OutputNodeId, Connection->OutputPin);
		OutSource += FString("#define ") + Define + " 1\n";

		if (bVector4 && Type == EMaterialValueType::Vector3)
		{
			OutSource += "    Result." + FString(Field) + " = float4(" + Variable + ", 1.0f);\n";
		}
		else if (bVector3 && Type == EMaterialValueType::Vector4)
		{
			OutSource += "    Result." + FString(Field) + " = " + Variable + ".rgb;\n";
		}
		else
		{
			OutSource += "    Result." + FString(Field) + " = " + Variable + ";\n";
		}
	}

	bool GenerateMaterialGraphSource(const UMaterial& Material, FString& OutSource, FString& OutError)
	{
		if (!Material.ValidateExpressionGraph(OutError))
		{
			return false;
		}

		std::unordered_map<int32, int32> TextureIndices;
		std::unordered_map<FString, int32> ParameterIndices;
		TArray<int32> TextureNodeIds;
		TArray<FMaterialGraphParameter> GraphParameters;

		Material.GetGraphParameterLayout(GraphParameters);
		for (int32 Index = 0; Index < GraphParameters.Num(); ++Index)
		{
			const FMaterialGraphParameter& Parameter = GraphParameters[Index];
			ParameterIndices[GetMaterialGraphParameterKey(Parameter.Name, Parameter.Type)] = Index;
		}

		TArray<int32> ReachableNodeIds;
		Material.GetReachableExpressionNodeIds(ReachableNodeIds);
		std::sort(ReachableNodeIds.begin(), ReachableNodeIds.end());

		for (int32 NodeId : ReachableNodeIds)
		{
			const FMaterialExpressionNode* Found = FindGraphNode(Material, NodeId);
			if (!Found)
			{
				continue;
			}
			const FMaterialExpressionNode& Node = *Found;
			if (Node.Type != EMaterialExpressionType::TextureSample &&
				Node.Type != EMaterialExpressionType::TextureSampleParameter2D)
			{
				continue;
			}
			TextureIndices[Node.Id] = TextureNodeIds.Num();
			TextureNodeIds.Add(Node.Id);
		}

		if (TextureNodeIds.Num() > MaxMaterialGraphTextures ||
			GraphParameters.Num() > MaxMaterialGraphParameters)
		{
			OutError = "Material Graph resource limit exceeded.";
			return false;
		}

		TArray<const FMaterialExpressionNode*> EvaluationOrder;
		std::unordered_map<int32, uint8> VisitState;
		if (!VisitGraphEvaluationOrder(Material, Material.OutputNodeId, VisitState, EvaluationOrder))
		{
			OutError = "Failed to build Material Graph evaluation order.";
			return false;
		}

		OutSource.clear();
		OutSource += "struct FMaterialGraphOutput\n{\n";
		OutSource += "    float4 BaseColor;\n";
		OutSource += "    float3 SpecularColor;\n";
		OutSource += "    float Shininess;\n";
		OutSource += "    float3 EmissiveColor;\n";
		OutSource += "    float EmissiveFactor;\n";
		OutSource += "    float Metallic;\n";
		OutSource += "    float Roughness;\n";
		OutSource += "    float OpacityMask;\n";
		OutSource += "    float3 Normal;\n";
		OutSource += "    float AmbientOcclusion;\n};\n";

		OutSource += "cbuffer MaterialGraphParameters : register(b4)\n{\n    float4 GraphParameterValues[16];\n};\n";
		for (int32 Index = 0; Index < TextureNodeIds.Num(); ++Index)
		{
			OutSource += "Texture2D GraphTexture" + std::to_string(Index) + " : register(t" + std::to_string(Index + 2) + ");\n";
		}

		for (const FMaterialExpressionNode* Node : EvaluationOrder)
		{
			if (Node->Type != EMaterialExpressionType::Custom)
			{
				continue;
			}
			const FMaterialExpressionConnection* InputA = FindGraphInput(Material, Node->Id, 0);
			const FMaterialExpressionConnection* InputB = FindGraphInput(Material, Node->Id, 1);
			if (!InputA || !InputB)
			{
				OutError = "Custom Expression requires two connected inputs.";
				return false;
			}
			const EMaterialValueType TypeA = Material.GetExpressionOutputType(InputA->OutputNodeId, InputA->OutputPin);
			const EMaterialValueType TypeB = Material.GetExpressionOutputType(InputB->OutputNodeId, InputB->OutputPin);
			const char* HlslTypeA = GetMaterialHlslType(TypeA);
			const char* HlslTypeB = GetMaterialHlslType(TypeB);
			const char* HlslOutputType = GetMaterialHlslType(Node->CustomOutputType);
			if (!HlslTypeA || !HlslTypeB || !HlslOutputType)
			{
				OutError = "Custom Expression has an unsupported input or output type.";
				return false;
			}
			OutSource += FString(HlslOutputType) + " EvaluateCustomNode_" + std::to_string(Node->Id);
			OutSource += "(" + FString(HlslTypeA) + " A, " + FString(HlslTypeB) + " B)\n{\n    return (" + Node->CustomCode + ");\n}\n";
		}

		OutSource += "float MaterialGraphHash21(float2 P)\n{\n    P = frac(P * float2(123.34f, 456.21f));\n    P += dot(P, P + 45.32f);\n    return frac(P.x * P.y);\n}\n";
		OutSource += "float MaterialGraphValueNoise(float2 P)\n{\n    float2 I = floor(P);\n    float2 F = frac(P);\n    F = F * F * (3.0f - 2.0f * F);\n";
		OutSource += "    float A = MaterialGraphHash21(I);\n    float B = MaterialGraphHash21(I + float2(1.0f, 0.0f));\n    float C = MaterialGraphHash21(I + float2(0.0f, 1.0f));\n    float D = MaterialGraphHash21(I + float2(1.0f, 1.0f));\n";
		OutSource += "    return lerp(lerp(A, B, F.x), lerp(C, D, F.x), F.y);\n}\n";
		OutSource += "float MaterialGraphNoise2D(float2 P)\n{\n    return MaterialGraphValueNoise(P) * 0.65f + MaterialGraphValueNoise(P * 2.03f) * 0.35f;\n}\n";

		OutSource += "FMaterialGraphOutput EvaluateMaterialGraph(float2 UV, float3 WorldPosition, float3 CameraVectorWS, float3 VertexNormalWS, float4 VertexColor, float Time)\n{\n";
		OutSource += "    FMaterialGraphOutput Result = (FMaterialGraphOutput)0;\n";

		for (const FMaterialExpressionNode* Node : EvaluationOrder)
		{
			const FString Variable = GraphNodeVariable(Node->Id);
			const EMaterialValueType OutputType = Material.GetExpressionOutputType(Node->Id, 0);
			const char* HlslType = GetMaterialHlslType(OutputType);
			if (!HlslType)
			{
				OutError = "Cannot resolve expression output type for node " + std::to_string(Node->Id);
				return false;
			}

			const FString A = GetGraphInputVariable(Material, Node->Id, 0, OutputType);
			const FString B = GetGraphInputVariable(Material, Node->Id, 1, OutputType);
			const FString C = GetGraphInputVariable(Material, Node->Id, 2, OutputType);

			switch (Node->Type)
			{
			case EMaterialExpressionType::Constant:
				OutSource += "    float " + Variable + " = " + HlslFloat(Node->ScalarValue) + ";\n";
				break;
			case EMaterialExpressionType::Constant2Vector:
				OutSource += "    float2 " + Variable + " = float2(" + HlslFloat(Node->VectorValue.X) + ", " + HlslFloat(Node->VectorValue.Y) + ");\n";
				break;
			case EMaterialExpressionType::Constant3Vector:
				OutSource += "    float3 " + Variable + " = float3(" + HlslFloat(Node->VectorValue.X) + ", " + HlslFloat(Node->VectorValue.Y) + ", " + HlslFloat(Node->VectorValue.Z) + ");\n";
				break;
			case EMaterialExpressionType::Constant4Vector:
				OutSource += "    float4 " + Variable + " = float4(" + HlslFloat(Node->VectorValue.X) + ", " + HlslFloat(Node->VectorValue.Y) + ", " + HlslFloat(Node->VectorValue.Z) + ", " + HlslFloat(Node->VectorValue.W) + ");\n";
				break;
			case EMaterialExpressionType::ScalarParameter:
			case EMaterialExpressionType::VectorParameter:
			{
				const FString Key = GetMaterialGraphParameterKey(Node->ParameterName, Node->Type);
				const std::unordered_map<FString, int32>::const_iterator It = ParameterIndices.find(Key);
				if (It == ParameterIndices.end())
				{
					OutError = "Material Graph parameter index was not generated.";
					return false;
				}
				if (Node->Type == EMaterialExpressionType::ScalarParameter)
					OutSource += "    float " + Variable + " = GraphParameterValues[" + std::to_string(It->second) + "].x;\n";
				else
					OutSource += "    float4 " + Variable + " = GraphParameterValues[" + std::to_string(It->second) + "];\n";
				break;
			}
			case EMaterialExpressionType::TextureCoordinate:
				OutSource += "    float2 " + Variable + " = UV;\n";
				break;
			case EMaterialExpressionType::TextureSample:
			case EMaterialExpressionType::TextureSampleParameter2D:
			{
				const std::unordered_map<int32, int32>::const_iterator It = TextureIndices.find(Node->Id);
				if (It == TextureIndices.end())
				{
					OutError = "Material Graph texture index was not generated.";
					return false;
				}
				const FString UVInput = GetGraphInputVariable(Material, Node->Id, 0);
				OutSource += "    float4 " + Variable + " = GraphTexture" + std::to_string(It->second) + ".Sample(ColorSampler, " + UVInput + ");\n";
				if (Node->bSRGB)
					OutSource += "    " + Variable + ".rgb = pow(max(" + Variable + ".rgb, 0.0f), 2.2f);\n";
				break;
			}
			case EMaterialExpressionType::Time:
				OutSource += "    float " + Variable + " = Time;\n";
				break;
			case EMaterialExpressionType::VertexColor:
				OutSource += "    float4 " + Variable + " = VertexColor;\n";
				break;
			case EMaterialExpressionType::WorldPosition:
				OutSource += "    float3 " + Variable + " = WorldPosition;\n";
				break;
			case EMaterialExpressionType::CameraVectorWS:
				OutSource += "    float3 " + Variable + " = CameraVectorWS;\n";
				break;
			case EMaterialExpressionType::VertexNormalWS:
				OutSource += "    float3 " + Variable + " = normalize(VertexNormalWS);\n";
				break;
			case EMaterialExpressionType::Add:
				OutSource += "    " + FString(HlslType) + " " + Variable + " = " + A + " + " + B + ";\n";
				break;
			case EMaterialExpressionType::Subtract:
				OutSource += "    " + FString(HlslType) + " " + Variable + " = " + A + " - " + B + ";\n";
				break;
			case EMaterialExpressionType::Multiply:
				OutSource += "    " + FString(HlslType) + " " + Variable + " = " + A + " * " + B + ";\n";
				break;
			case EMaterialExpressionType::Divide:
				OutSource += "    " + FString(HlslType) + " " + Variable + " = " + A + " / " + B + ";\n";
				break;
			case EMaterialExpressionType::Lerp:
				OutSource += "    " + FString(HlslType) + " " + Variable + " = lerp(" + A + ", " + B + ", " + GetGraphInputVariable(Material, Node->Id, 2) + ");\n";
				break;
			case EMaterialExpressionType::OneMinus:
				OutSource += "    " + FString(HlslType) + " " + Variable + " = 1.0f - " + GetGraphInputVariable(Material, Node->Id, 0) + ";\n";
				break;
			case EMaterialExpressionType::Step:
				OutSource += "    float " + Variable + " = step(" + A + ", " + B + ");\n";
				break;
			case EMaterialExpressionType::Noise:
				OutSource += "    float " + Variable + " = MaterialGraphNoise2D(" + A + ");\n";
				break;
			case EMaterialExpressionType::Clamp:
				OutSource += "    " + FString(HlslType) + " " + Variable + " = clamp(" + A + ", " + GetGraphInputVariable(Material, Node->Id, 1, OutputType) + ", " + GetGraphInputVariable(Material, Node->Id, 2, OutputType) + ");\n";
				break;
			case EMaterialExpressionType::Power:
				OutSource += "    " + FString(HlslType) + " " + Variable + " = pow(" + A + ", " + B + ");\n";
				break;
			case EMaterialExpressionType::Abs:
				OutSource += "    " + FString(HlslType) + " " + Variable + " = abs(" + GetGraphInputVariable(Material, Node->Id, 0) + ");\n";
				break;
			case EMaterialExpressionType::Frac:
				OutSource += "    " + FString(HlslType) + " " + Variable + " = frac(" + GetGraphInputVariable(Material, Node->Id, 0) + ");\n";
				break;
			case EMaterialExpressionType::Sine:
				OutSource += "    " + FString(HlslType) + " " + Variable + " = sin(" + GetGraphInputVariable(Material, Node->Id, 0) + ");\n";
				break;
			case EMaterialExpressionType::Min:
				OutSource += "    " + FString(HlslType) + " " + Variable + " = min(" + A + ", " + B + ");\n";
				break;
			case EMaterialExpressionType::Max:
				OutSource += "    " + FString(HlslType) + " " + Variable + " = max(" + A + ", " + B + ");\n";
				break;
			case EMaterialExpressionType::DotProduct:
				OutSource += "    float " + Variable + " = dot(" + GetGraphInputVariable(Material, Node->Id, 0) + ", " + GetGraphInputVariable(Material, Node->Id, 1) + ");\n";
				break;
			case EMaterialExpressionType::Normalize:
				OutSource += "    " + FString(HlslType) + " " + Variable + " = normalize(" + GetGraphInputVariable(Material, Node->Id, 0) + ");\n";
				break;
			case EMaterialExpressionType::AppendVector:
				OutSource += "    " + FString(HlslType) + " " + Variable + " = " + FString(HlslType) + "(" + GetGraphInputVariable(Material, Node->Id, 0) + ", " + GetGraphInputVariable(Material, Node->Id, 1) + ");\n";
				break;
			case EMaterialExpressionType::Custom:
				OutSource += "    " + FString(HlslType) + " " + Variable + " = EvaluateCustomNode_" + std::to_string(Node->Id) + "(" + GetGraphInputVariable(Material, Node->Id, 0) + ", " + GetGraphInputVariable(Material, Node->Id, 1) + ");\n";
				break;
			case EMaterialExpressionType::ComponentMask:
			{
				FString Swizzle;
				static const char Channels[] = { 'r', 'g', 'b', 'a' };
				for (int32 Channel = 0; Channel < 4; ++Channel)
				{
					if ((Node->ComponentMask & (1u << Channel)) != 0)
						Swizzle += Channels[Channel];
				}
				const FString Input = GetGraphInputVariable(Material, Node->Id, 0);
				OutSource += "    " + FString(HlslType) + " " + Variable + " = " + Input + "." + Swizzle + ";\n";
				break;
			}
			case EMaterialExpressionType::Saturate:
				OutSource += "    " + FString(HlslType) + " " + Variable + " = saturate(" + GetGraphInputVariable(Material, Node->Id, 0) + ");\n";
				break;
			case EMaterialExpressionType::Panner:
				OutSource += "    float2 " + Variable + " = " + A + " + Time * " + B + ";\n";
				break;
			case EMaterialExpressionType::Fresnel:
				OutSource += "    float " + Variable + " = " + C + " + (1.0f - " + C + ") * pow(1.0f - saturate(dot(normalize(" + GetGraphInputVariable(Material, Node->Id, 0) + "), normalize(CameraVectorWS))), " + B + ");\n";
				break;
			case EMaterialExpressionType::Cosine:
				OutSource += "    " + FString(HlslType) + " " + Variable + " = cos(" + GetGraphInputVariable(Material, Node->Id, 0) + ");\n";
				break;
			case EMaterialExpressionType::Floor:
				OutSource += "    " + FString(HlslType) + " " + Variable + " = floor(" + GetGraphInputVariable(Material, Node->Id, 0) + ");\n";
				break;
			case EMaterialExpressionType::Distance:
				OutSource += "    float " + Variable + " = distance(" + GetGraphInputVariable(Material, Node->Id, 0) + ", " + GetGraphInputVariable(Material, Node->Id, 1) + ");\n";
				break;
			case EMaterialExpressionType::Length:
				OutSource += "    float " + Variable + " = length(" + GetGraphInputVariable(Material, Node->Id, 0) + ");\n";
				break;
			case EMaterialExpressionType::CrossProduct:
				OutSource += "    float3 " + Variable + " = cross(" + GetGraphInputVariable(Material, Node->Id, 0) + ", " + GetGraphInputVariable(Material, Node->Id, 1) + ");\n";
				break;
			case EMaterialExpressionType::Desaturation:
			{
				const FString Color = GetGraphInputVariable(Material, Node->Id, 0);
				const FString Fraction = GetGraphInputVariable(Material, Node->Id, 1);
				const FString Gray = Variable + "_Gray";
				OutSource += "    float " + Gray + " = dot(" + Color + ".rgb, float3(0.2126f, 0.7152f, 0.0722f));\n";
				if (OutputType == EMaterialValueType::Vector4)
					OutSource += "    float4 " + Variable + " = float4(lerp(" + Color + ".rgb, float3(" + Gray + ", " + Gray + ", " + Gray + "), saturate(" + Fraction + ")), " + Color + ".a);\n";
				else
					OutSource += "    float3 " + Variable + " = lerp(" + Color + ", float3(" + Gray + ", " + Gray + ", " + Gray + "), saturate(" + Fraction + "));\n";
				break;
			}
			case EMaterialExpressionType::SphereMask:
			{
				const FString Radius = GetGraphInputVariable(Material, Node->Id, 2);
				const FString Softness = GetGraphInputVariable(Material, Node->Id, 3);
				OutSource += "    float " + Variable + " = 1.0f - smoothstep(" + Radius + " - max(" + Softness + ", 0.0001f), " + Radius + ", distance(" + A + ", " + B + "));\n";
				break;
			}
			case EMaterialExpressionType::ConstantBiasScale:
				OutSource += "    " + FString(HlslType) + " " + Variable + " = (" + A + " + " + B + ") * " + C + ";\n";
				break;
			case EMaterialExpressionType::Rotator:
			{
				const FString SinValue = Variable + "_Sin";
				const FString CosValue = Variable + "_Cos";
				const FString Offset = Variable + "_Offset";
				OutSource += "    float " + SinValue + ";\n    float " + CosValue + ";\n";
				OutSource += "    sincos(" + GetGraphInputVariable(Material, Node->Id, 2) + ", " + SinValue + ", " + CosValue + ");\n";
				OutSource += "    float2 " + Offset + " = " + A + " - " + B + ";\n";
				OutSource += "    float2 " + Variable + " = float2(" + Offset + ".x * " + CosValue + " - " + Offset + ".y * " + SinValue + ", " + Offset + ".x * " + SinValue + " + " + Offset + ".y * " + CosValue + ") + " + B + ";\n";
				break;
			}
			default:
				OutError = "Unsupported Material Expression.";
				return false;
			}
		}

		OutSource += "    Result.BaseColor = float4(1, 1, 1, 1);\n";
		OutSource += "    Result.SpecularColor = float3(1, 1, 1);\n";
		OutSource += "    Result.Shininess = 32.0f;\n";
		OutSource += "    Result.EmissiveColor = float3(0, 0, 0);\n";
		OutSource += "    Result.EmissiveFactor = 1.0f;\n";
		OutSource += "    Result.Metallic = 0.0f;\n";
		OutSource += "    Result.Roughness = 0.8f;\n";
		OutSource += "    Result.OpacityMask = 1.0f;\n";
		OutSource += "    Result.Normal = float3(0, 0, 1);\n";
		OutSource += "    Result.AmbientOcclusion = 1.0f;\n";

		EmitGraphOutputPin(Material, 0, "MATERIAL_GRAPH_HAS_BASE_COLOR", "BaseColor", true, false, OutSource);
		EmitGraphOutputPin(Material, 1, "MATERIAL_GRAPH_HAS_SPECULAR_COLOR", "SpecularColor", false, true, OutSource);
		EmitGraphOutputPin(Material, 2, "MATERIAL_GRAPH_HAS_SHININESS", "Shininess", false, false, OutSource);
		EmitGraphOutputPin(Material, 3, "MATERIAL_GRAPH_HAS_EMISSIVE_COLOR", "EmissiveColor", false, true, OutSource);
		EmitGraphOutputPin(Material, 4, "MATERIAL_GRAPH_HAS_EMISSIVE_FACTOR", "EmissiveFactor", false, false, OutSource);
		EmitGraphOutputPin(Material, 5, "MATERIAL_GRAPH_HAS_METALLIC", "Metallic", false, false, OutSource);
		EmitGraphOutputPin(Material, 6, "MATERIAL_GRAPH_HAS_ROUGHNESS", "Roughness", false, false, OutSource);
		EmitGraphOutputPin(Material, 7, "MATERIAL_GRAPH_HAS_OPACITY_MASK", "OpacityMask", false, false, OutSource);
		EmitGraphOutputPin(Material, 8, "MATERIAL_GRAPH_HAS_NORMAL", "Normal", false, true, OutSource);
		EmitGraphOutputPin(Material, 9, "MATERIAL_GRAPH_HAS_AMBIENT_OCCLUSION", "AmbientOcclusion", false, false, OutSource);

		OutSource += "    return Result;\n}\n";
		OutSource += "FMaterialGraphOutput EvaluateMaterialGraph(float2 UV)\n{\n";
		OutSource += "    return EvaluateMaterialGraph(UV, float3(0, 0, 0), float3(0, 0, 1), float3(0, 0, 1), float4(1, 1, 1, 1), 0.0f);\n}\n";

		OutError.clear();
		return true;
	}

	FString GetMaterialGraphHash(const FString& Source)
	{
		uint64 Hash = 14695981039346656037ull;
		for (unsigned char Value : Source)
		{
			Hash ^= Value;
			Hash *= 1099511628211ull;
		}
		std::ostringstream Stream;
		Stream << std::hex << Hash;
		return Stream.str();
	}

	FString GetMaterialGraphAssetId(const UMaterial& Material)
	{
		FString Identity = Material.GetPath();
		if (Identity.empty())
		{
			Identity = std::to_string(reinterpret_cast<uintptr_t>(&Material));
		}
		return GetMaterialGraphHash(Identity);
	}

	bool SaveGeneratedMaterialGraph(const FString& Source, const FString& AssetId, FString& OutIncludePath)
	{
		const fs::path Directory = FPaths::EngineFile("EngineResources/Shaders/GeneratedMaterial");
		std::error_code Error;
		fs::create_directories(Directory, Error);
		if (Error)
		{
			return false;
		}

		const fs::path File = Directory / ("Graph_" + AssetId + ".hlsli");
		std::ifstream Existing(File, std::ios::binary);
		const FString ExistingSource(std::istreambuf_iterator<char>{ Existing }, std::istreambuf_iterator<char>{});
		if (ExistingSource != Source)
		{
			std::ofstream Output(File, std::ios::binary | std::ios::trunc);
			if (!Output)
			{
				return false;
			}
			Output.write(Source.data(), static_cast<std::streamsize>(Source.size()));
			if (!Output)
			{
				return false;
			}
		}

		OutIncludePath = "GeneratedMaterial/Graph_" + AssetId + ".hlsli";
		return true;
	}

	bool SaveGeneratedMaterialShaderVariant(const FString& BaseShaderPath, const FString& IncludePath, const FString& AssetId, FString& OutShaderPath)
	{
		const fs::path ResolvedBase = FPaths::ResolveEngineRelative(BaseShaderPath);
		std::ifstream Input(ResolvedBase, std::ios::binary);
		if (!Input)
		{
			return false;
		}

		const FString BaseSource(std::istreambuf_iterator<char>{ Input }, std::istreambuf_iterator<char>{});
		const FString IncludeMarker = "#include MATERIAL_GRAPH_INCLUDE";
		const size_t IncludePosition = BaseSource.find(IncludeMarker);
		if (IncludePosition == FString::npos)
		{
			return false;
		}

		FString GeneratedSource = BaseSource;
		const FString LiteralInclude = "#include \"" + IncludePath + "\"";
		GeneratedSource.replace(IncludePosition, IncludeMarker.size(), LiteralInclude);

		const fs::path VariantFile = ResolvedBase.parent_path() / (ResolvedBase.stem().string() + "_Graph_" + AssetId + ".hlsli");
		std::ifstream Existing(VariantFile, std::ios::binary);
		const FString ExistingSource(std::istreambuf_iterator<char>{ Existing }, std::istreambuf_iterator<char>{});
		if (ExistingSource != GeneratedSource)
		{
			std::ofstream Output(VariantFile, std::ios::binary | std::ios::trunc);
			if (!Output)
			{
				return false;
			}
			Output.write(GeneratedSource.data(), static_cast<std::streamsize>(GeneratedSource.size()));
			if (!Output)
			{
				return false;
			}
		}

		const fs::path RelBase(BaseShaderPath);
		OutShaderPath = (RelBase.parent_path() / (RelBase.stem().string() + "_Graph_" + AssetId + ".hlsli")).generic_string();
		return true;
	}

	bool ResolveMaterialGraphShaderPath(EPSOType TargetPSO, EVertexFactoryType VFType, FString& OutShaderPath)
	{
		if (VFType == EVertexFactoryType::Trail)
		{
			OutShaderPath = "EngineResources/Shaders/ParticleTrailShader.hlsl";
			return true;
		}
		if (VFType == EVertexFactoryType::GpuSpriteParticle)
		{
			OutShaderPath = "EngineResources/Shaders/ParticleGpuInstancedShader.hlsl";
			return true;
		}
		if (VFType == EVertexFactoryType::GpuMeshParticle)
		{
			OutShaderPath = "EngineResources/Shaders/ParticleGpuMeshInstancedShader.hlsl";
			return true;
		}
		switch (TargetPSO)
		{
		case EPSOType::StaticMesh_Forward:
			OutShaderPath = "EngineResources/Shaders/UberLit.hlsl";
			return true;
		case EPSOType::StaticMesh_Translucent:
		case EPSOType::StaticMesh_Additive:
			OutShaderPath = "EngineResources/Shaders/StaticMeshTranslucentShader.hlsl";
			return true;
		case EPSOType::Particle_AlphaBlend:
		case EPSOType::Particle_Additive:
			OutShaderPath = "EngineResources/Shaders/ParticleSpriteShader.hlsl";
			return true;
		default:
			return false;
		}
	}

	FString BuildMaterialGraphProgramKey(const FString& ShaderPath, const FString& Hash, EPSOType TargetPSO,
		ELightingModel LightingModel, EVertexFactoryType VFType)
	{
		FString Key = ShaderPath + "#MaterialGraph_" + Hash + "#PSO_" + std::to_string(static_cast<uint8>(TargetPSO));
		if (TargetPSO == EPSOType::StaticMesh_Forward)
		{
			Key += "#LM_" + std::to_string(static_cast<int32>(LightingModel));
		}
		Key += "#" + FString(GetVertexFactoryTypeInfo(VFType).Name);
		return Key;
	}

	FShaderProgram* LookupCachedMaterialGraphProgram(
		TMap<FString, TUniquePtr<FShaderProgram>>& ProgramMap,
		TMap<FString, FString>& ErrorMap,
		const FString& Key,
		FString& OutError,
		bool& bOutTerminal)
	{
		bOutTerminal = false;
		if (TUniquePtr<FShaderProgram>* Found = ProgramMap.Find(Key))
		{
			OutError.clear();
			bOutTerminal = true;
			return Found->get();
		}
		if (FString* PreviousError = ErrorMap.Find(Key))
		{
			OutError = *PreviousError;
			bOutTerminal = true;
			return nullptr;
		}
		return nullptr;
	}
}

FShaderProgram* FRenderResourceManager::GetMaterialGraphShaderProgram(const UMaterial& Material, EPSOType TargetPSO,
	ELightingModel LightingModel, EVertexFactoryType VFType, FString& OutError)
{
	FString ShaderPath;
	if (!ResolveMaterialGraphShaderPath(TargetPSO, VFType, ShaderPath))
	{
		OutError = "Unsupported Material Graph PSO.";
		return nullptr;
	}

	const uint64 GraphSignature = Material.ComputeGraphSignature();
	FString Hash;
	bool bCachedGraphOk = false;
	FString CachedError;
	const bool bHasCachedHash = Material.GetCachedGraphHash(GraphSignature, Hash, bCachedGraphOk, CachedError);

	FString Key;
	if (bHasCachedHash)
	{
		if (!bCachedGraphOk)
		{
			OutError = CachedError;
			return nullptr;
		}

		Key = BuildMaterialGraphProgramKey(ShaderPath, Hash, TargetPSO, LightingModel, VFType);
		bool bTerminal = false;
		FShaderProgram* CachedProgram = LookupCachedMaterialGraphProgram(Get().ShaderProgramMap, Get().MaterialGraphErrorMap, Key, OutError, bTerminal);
		if (bTerminal)
		{
			return CachedProgram;
		}
	}

	FString GeneratedSource;
	if (!GenerateMaterialGraphSource(Material, GeneratedSource, OutError))
	{
		Material.SetCachedGraphHash(GraphSignature, FString(), false, OutError);
		return nullptr;
	}

	if (!bHasCachedHash)
	{
		Hash = GetMaterialGraphHash(GeneratedSource);
		Material.SetCachedGraphHash(GraphSignature, Hash, true, FString());
		Key = BuildMaterialGraphProgramKey(ShaderPath, Hash, TargetPSO, LightingModel, VFType);

		bool bTerminal = false;
		FShaderProgram* CachedProgram = LookupCachedMaterialGraphProgram(Get().ShaderProgramMap, Get().MaterialGraphErrorMap, Key, OutError, bTerminal);
		if (bTerminal)
		{
			return CachedProgram;
		}
	}

	FShaderProgram* BaseForPSO = nullptr;
	if (TargetPSO == EPSOType::StaticMesh_Forward)
	{
		BaseForPSO = GetForwardShaderProgram(LightingModel);
	}
	else if (FPipelineState* Pipeline = GetPSO(TargetPSO))
	{
		BaseForPSO = Pipeline->Shader;
	}

	FShaderProgram* BaseProgram = ResolveShader(BaseForPSO, VFType);
	if (!BaseProgram || !BaseProgram->VertexShader)
	{
		OutError = "Base Material Shader is not available.";
		return nullptr;
	}

	const FString AssetId = GetMaterialGraphAssetId(Material);
	FString IncludePath;
	if (!SaveGeneratedMaterialGraph(GeneratedSource, AssetId, IncludePath))
	{
		OutError = "Failed to write generated Material Graph HLSL.";
		return nullptr;
	}

	FString GeneratedShaderPath;
	if (!SaveGeneratedMaterialShaderVariant(ShaderPath, IncludePath, AssetId, GeneratedShaderPath))
	{
		OutError = "Failed to generate Material Graph shader variant.";
		return nullptr;
	}

	const FString UniqueDefine = "MATERIAL_GRAPH_HASH_" + Hash;
	D3D_SHADER_MACRO Defines[6]{};
	int32 DefineCount = 0;

	if (TargetPSO == EPSOType::StaticMesh_Forward)
	{
		const char* LightMacro = "LIGHTING_MODEL_PHONG";
		switch (LightingModel)
		{
		case ELightingModel::Gouraud: LightMacro = "LIGHTING_MODEL_GOURAUD"; break;
		case ELightingModel::Lambert: LightMacro = "LIGHTING_MODEL_LAMBERT"; break;
		case ELightingModel::Unlit: LightMacro = "LIGHTING_MODEL_UNLIT"; break;
		default: break;
		}
		Defines[DefineCount++] = { LightMacro, "1" };
	}

	const FVertexFactoryTypeInfo& VFInfo = GetVertexFactoryTypeInfo(VFType);
	if (VFInfo.Define && VFInfo.Elements.size() > 0)
	{
		Defines[DefineCount++] = { VFInfo.Define, "1" };
	}

	Defines[DefineCount++] = { "MATERIAL_GRAPH", "1" };
	Defines[DefineCount++] = { UniqueDefine.c_str(), "1" };
	Defines[DefineCount] = { nullptr, nullptr };

	FString CsoPath;
	FShaderByteCode PixelCode = RenderUtil::GetOrCompile(GeneratedShaderPath, "mainPS", EShaderType::Pixel, CsoPath, Defines);
	if (!PixelCode.IsValid())
	{
		OutError = "[MaterialGraph] Pixel Shader compilation failed: " + Key;
		Get().MaterialGraphErrorMap[Key] = OutError;
		KRS_LOG(Error, "{}", OutError);
		return nullptr;
	}

	TUniquePtr<FPixelShader> PixelShader = RenderCommand::CreatePixelShader(PixelCode);
	if (!PixelShader || !PixelShader->IsValid())
	{
		OutError = "[MaterialGraph] Failed to create Pixel Shader: " + Key;
		Get().MaterialGraphErrorMap[Key] = OutError;
		KRS_LOG(Error, "{}", OutError);
		return nullptr;
	}

	FPixelShader* PixelRaw = PixelShader.get();
	Get().PixelShaderMap[Key + "#PS"] = std::move(PixelShader);
	Get().ShaderProgramMap[Key] = MakeUnique<FShaderProgram>(BaseProgram->VertexShader, PixelRaw);
	Get().ShaderProgramMap[Key]->bUsesVertexFactory = BaseProgram->bUsesVertexFactory;

	OutError.clear();
	return Get().ShaderProgramMap[Key].get();
}

FShaderProgram* FRenderResourceManager::ResolveShader(FShaderProgram* Base, EVertexFactoryType VF)
{
	if (!Base) return nullptr;
	// 빔, 리본: 파티클 머티리얼의 블렌드와 깊이 설정은 PSO 그대로 두고 셰이더만 정점 형식에 맞는 것으로 바꾼다
	if (VF == EVertexFactoryType::Trail)
	{
		return Base->Name == "EngineResources/Shaders/ParticleSpriteShader.hlsl"
			? FindShaderProgram("EngineResources/Shaders/ParticleTrailShader.hlsl")
			: nullptr;
	}
	if (VF == EVertexFactoryType::GpuSpriteParticle)
	{
		return Base->Name == "EngineResources/Shaders/ParticleSpriteShader.hlsl"
			? FindShaderProgram("EngineResources/Shaders/ParticleGpuInstancedShader.hlsl")
			: nullptr;
	}
	if (VF == EVertexFactoryType::GpuMeshParticle)
	{
		return (Base->Name == "EngineResources/Shaders/UberLit.hlsl" ||
			Base->Name == "EngineResources/Shaders/StaticMeshTranslucentShader.hlsl")
			? FindShaderProgram("EngineResources/Shaders/ParticleGpuMeshInstancedShader.hlsl")
			: nullptr;
	}
	// 전용 셰이더는 자기 입력 하나만 받는다
	const bool bDedicatedVF = GetVertexFactoryTypeInfo(VF).Elements.empty();
	if (!Base->bUsesVertexFactory)
		return VF == EVertexFactoryType::Local || bDedicatedVF ? Base : nullptr;
	if (bDedicatedVF) return nullptr;
	if (VF == EVertexFactoryType::Local) return Base;
	return FindShaderProgram(MakeVFKey(Base->Name, VF));
}
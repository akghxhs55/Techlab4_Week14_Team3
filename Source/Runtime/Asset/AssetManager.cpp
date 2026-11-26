#include "EnginePCH.h"
#include "AssetManager.h"
#include "Render/Buffer.h"
#include "Render/Material.h"
#include "Render/MaterialInstance.h"
#include "Render/StockingMaterial.h"
#include "Serialization/JsonArchive.h"
#include <fstream>
#include <set>

#include "Render/Vertex.h"
#include "Render/Texture2D.h"
#include "Render/RenderCommand.h"

#include "Render/RenderResourceManager.h"

#include "Asset/ObjImporter/ObjImporter.h"
#include "Asset/FbxImporter/FbxMeshBake.h"
#include "Render/SkeletalMesh.h"

#include "Render/GeometryGenerator.h"
#include "UObject/UObjectGlobals.h"

#include "Audio/SoundWave.h"

#include "Animation/AnimSequence.h"
#include "Animation/AnimSequenceFile.h"
#include "Animation/BlendSpace1D.h"
#include "Animation/BlendSpace2D.h"
#include "Animation/BlendSpaceFile.h"
#include "Particles/ParticleSystem.h"
#include "Particles/ParticleSystemFile.h"
#include "Asset/AssetUtils.h"
#include "Physics/PhysicsAsset.h"
#include "Physics/PhysicsAssetFile.h"
#include "Physics/BodySetup.h"
#include "Render/ImageLoader.h"
#include "Asset/AssetLoadProfiler.h"
#include "Core/Misc/Paths.h"
#include <cctype>
#include <limits>

namespace
{
	FString GetAssetExtension(const FString& Path) {
		const std::u8string UTF8Path{ Path.begin(), Path.end() };
		FString Extension{ std::filesystem::path{ UTF8Path }.extension().string() };
		for (char& Character : Extension) {
			Character = static_cast<char>(std::tolower(static_cast<unsigned char>(Character)));
		}
		return Extension;
	}

	bool IsMeshSidecarJsonPath(const FString& Path) {
		FString Lower = Path;
		for (char& Character : Lower) {
			Character = static_cast<char>(std::tolower(static_cast<unsigned char>(Character)));
		}
		return Lower.ends_with(".physics.json") || Lower.ends_with(".fbx.bin.sockets.json")
			|| Lower.ends_with(".fbx.bin.physicsasset.json");
	}

	EAssetType GetAssetTypeFromPath(const FString& Path) {
		const FString Extension{ GetAssetExtension(Path) };
		if (Extension == ".png" || Extension == ".dds") {
			return EAssetType::Texture;
		}
		if (Extension == ".json") {
			if (IsMeshSidecarJsonPath(Path)) {
				return EAssetType::None;
			}
			return EAssetType::Font;
		}
		if (Extension == ".obj") {
			return EAssetType::StaticMesh;
		}
		if (Extension == ".material") {
			return EAssetType::Material;
		}
		if (Extension == ".bin" && FFbxMeshBake::IsBakedPath(Path)) {
			// 파일을 읽지 않고 구분하도록 FFbxImporter의 저장 이름 규칙을 쓴다
			return Path.ends_with("_Static.fbx.bin") ? EAssetType::StaticMesh : EAssetType::SkeletalMesh;
		}
		return EAssetType::None;
	}

	// 엔진 콘텐츠로 옮긴 파일의 옛 키("Icons/Point.png")를 엔진 키("Engine/Icons/Point.png")로 바꾼다.
	// 옛 씬은 "Content/Game/Icons/..."를 저장하고 있으므로, 프로젝트에 그 파일이 없을 때 엔진 콘텐츠에서 찾는다.
	FString MakeEngineFallbackKey(const FString& Key)
	{
		if (Key.starts_with(FPaths::EngineContentPrefix)) return {};
		return FPaths::EngineContentPrefix + Key;
	}

	UStaticMesh* CreateStaticMesh(const FStaticMeshData* MeshData)
	{
		if (!MeshData || !MeshData->Vertices.Num() || !MeshData->Indices.Num())
			return nullptr;

		FLoadStageTimer CopyTimer{ "Mesh/CopyData" };
		FStaticMeshData Optimized = *MeshData;
		CopyTimer.Stop();

		// Mesh에 MaterialSlot 없을 경우 Default Material 할당
		if (Optimized.MaterialSlots.IsEmpty())
		{
			FStaticMaterialSlot DefaultSlot;
			DefaultSlot.Name = "Default";
			Optimized.MaterialSlots.Add(DefaultSlot);
		}

		// Mesh에 Section이 없을 경우 전체 메쉬을 섹션 하나로 세팅
		if (Optimized.Sections.IsEmpty())
		{
			FStaticMeshSection DefaultSection;
			DefaultSection.StartIndex = 0;
			DefaultSection.IndexCount = static_cast<uint32>(Optimized.Indices.Num());
			DefaultSection.MaterialSlotIndex = 0;

			Optimized.Sections.Add(DefaultSection);
		}

		{
			FLoadStageTimer Timer{ "Mesh/BuildTangentSpace" };
			Optimized.BuildTangentSpace();
		}
		{
			FLoadStageTimer Timer{ "Mesh/OptimizeTriangleOrder" };
			Optimized.OptimizeTriangleOrderForVertexCache();
		}

		FLoadStageTimer BufferTimer{ "Mesh/CreateGPUBuffers" };
		TUniquePtr<FVertexBuffer> VB = RenderCommand::CreateStaticVertexBuffer(Optimized.Vertices.GetData(), sizeof(FVertex) * static_cast<uint32>(Optimized.Vertices.size()), sizeof(FVertex));
		TUniquePtr<FIndexBuffer> IB = RenderCommand::CreateStaticIndexBuffer(Optimized.Indices.GetData(), static_cast<uint32>(Optimized.Indices.size()));
		BufferTimer.Stop();
		if (VB == nullptr || IB == nullptr) return nullptr;

		UStaticMesh* Mesh = NewObject<UStaticMesh>();
		Mesh->MeshData = std::move(Optimized);

		// Vertex/Index GPU 업로드
		Mesh->VertexBuffer = std::move(VB);
		Mesh->IndexBuffer = std::move(IB);

		// 빈 슬롯이면 위에서 Default 슬롯을 넣었으므로 원본(MeshData)이 아니라 Mesh->MeshData를 쓴다
		FLoadStageTimer MaterialTimer{ "Mesh/BuildMaterials (includes texture loads)" };
		if (!UAssetManager::BuildMaterialsFromSlots(Mesh->MeshData.MaterialSlots, Mesh->Materials))
		{
			return nullptr;
		}
		MaterialTimer.Stop();

		{
			FLoadStageTimer Timer{ "Mesh/BuildTriangleBVH" };
			Mesh->MeshData.BuildTriangleBVH();
		}
		{
			FLoadStageTimer Timer{ "Mesh/BuildOBBTree" };
			Mesh->MeshData.BuildOBBTree();
		}
		{
			FLoadStageTimer Timer{ "Mesh/RebuildRenderData" };
			Mesh->RebuildRenderData();
		}

		return Mesh;
	}

}

UAssetManager& UAssetManager::Get()
{
	static UAssetManager* Instance = NewObject<UAssetManager>();
	return *Instance;
}

void UAssetManager::Init(const FAssetLoadProgress& OnProgress)
{
	{
		FLoadStageTimer Timer{ "Init/Defaults before scan" };
		FGeometryGenerator::CreateDefaultMeshDatas();
		// 머티리얼이 참조하므로 반드시 먼저 만든다
		Get().CreateDefaultTextures();
		Get().CreateDefaultMaterial();
	}
	{
		FLoadStageTimer Timer{ "Init/ScanAssets" };
		Get().ScanAssets(FPaths::ProjectContentDir(), OnProgress);
		// 엔진 콘텐츠는 엔진 폴더에 있고 키는 "Engine/..."이다.
		Get().ScanAssets(FPaths::EngineResourcesDir(), OnProgress, FPaths::EngineContentPrefix);
	}
	FLoadStageTimer DefaultsTimer{ "Init/Defaults after scan" };
	Get().CreateDefaultMeshes();
	Get().CreateBillboardMaterial();
	Get().CreateParticleMaterial();
	Get().CreateEditorArrowMaterial();
}

void UAssetManager::ScanAssets(const fs::path& AssetRoot, const FAssetLoadProgress& OnProgress, const FString& KeyPrefix)
{
	if (!fs::exists(AssetRoot))
	{
		KRS_LOG(Error, "Asset root not found: {}", AssetRoot.generic_string());
		return;
	}

	FLoadStageTimer EnumerateTimer{ "Scan/EnumerateFiles" };
	TArray<fs::path> Files;
	for (const fs::directory_entry& Entry : fs::recursive_directory_iterator(AssetRoot))
	{
		if (Entry.is_regular_file())
		{
			Files.Add(Entry.path());
		}
	}
	EnumerateTimer.Stop();

	const int32 Total = Files.Num();
	for (int32 i = 0; i < Total; ++i)
	{
		FString Key = FPaths::ToUtf8(fs::relative(Files[i], AssetRoot));
		FString Path = FPaths::ToUtf8(Files[i]);
		if (!KeyPrefix.empty())
		{
			// 엔진 콘텐츠는 경로도 "Engine/..." 그대로 둔다. 파일을 열 때 FPaths::ResolveContentPath로 바꾼다.
			// 이렇게 해야 로드 뒤 RegisterAsset이 같은 키로 등록하고, 씬에도 "Engine/..."으로 저장된다.
			Key = KeyPrefix + FPaths::ToUtf8(Files[i].lexically_relative(AssetRoot));
			Path = Key;
		}

		// 경로만 등록한다. 실제 로드는 처음 찾을 때 FindOrLoadAsset에서 한다.
		if (GetAssetTypeFromPath(Path) != EAssetType::None)
		{
			AssetPathMap.Add(Key, Path);
		}

		if (OnProgress)
		{
			FLoadStageTimer Timer{ "Scan/OnProgress (splash)" };
			OnProgress(i + 1, Total, Key);
		}
	}
}

void UAssetManager::LoadAsset(const FString& Key, const FString& Path) {
	// FBX Asset은 원본 .fbx 대신 Import로 저장한 .fbx.bin만 로드한다.
	// 원본 해석과 변경 여부 비교는 FFbxImporter::ImportToAssets에서만 수행하며,
	// 이 로드 경로는 원본 파일이나 FBX SDK 없이 저장된 데이터로 렌더 자원을 만든다.
	if (FFbxMeshBake::IsBakedPath(Path)) {
		FString Error;
		if (!LoadFbxMesh(Path, Error)) KRS_LOG(Error, "{}", Error);
		return;
	}

	switch (GetAssetTypeFromPath(Path)) {
	case EAssetType::Texture:
		LoadTexture(Path);
		break;
	case EAssetType::Font:
		LoadFontAtlas(Path, fs::path{ Path }.replace_extension(".png").string());
		break;
	case EAssetType::StaticMesh:
		LoadObjStaticMesh(Path);
		break;
	case EAssetType::Material:
		LoadMaterialAsset(Path);
		break;
	default:
		break;
	}
}

URenderAsset* UAssetManager::FindOrLoadAsset(const FString& Path)
{
	// 렌더링 중에도 불리는 내장 에셋("DefaultMaterial" 등)은 키를 만들지 않고 바로 찾는다
	if (URenderAsset** Found = AssetMap.Find(Path))
	{
		return *Found;
	}

	const FString Key = FAssetUtils::GetAssetKey(Path);
	if (URenderAsset** Found = AssetMap.Find(Key))
	{
		return *Found;
	}

	const FString* FoundSource = AssetPathMap.Find(Key);
	if (FoundSource == nullptr)
	{
		// 프로젝트에 없으면 엔진 콘텐츠에서 찾는다. 프로젝트에 같은 경로의 파일이 있으면 그쪽이 먼저다.
		const FString EngineKey = MakeEngineFallbackKey(Key);
		if (!EngineKey.empty() && AssetPathMap.Contains(EngineKey))
		{
			return FindOrLoadAsset(EngineKey);
		}
		KRS_LOG(Warning, "Asset not found: {}", Path);
		return nullptr;
	}
	const FString SourcePath = *FoundSource;

	const uint64 LoadStart = FPlatformTime::GetCycles64();
	LoadAsset(Key, SourcePath);
	const double LoadMs = FPlatformTime::ToMilliseconds64(FPlatformTime::GetCycles64() - LoadStart);
	FAssetLoadProfiler::AddFile(SourcePath, LoadMs);
	FAssetLoadProfiler::AddStage("Lazy/LoadAsset " + GetAssetExtension(SourcePath), LoadMs);

	URenderAsset** Loaded = AssetMap.Find(Key);
	if (Loaded == nullptr)
	{
		// 찾을 때마다 파일을 다시 읽지 않도록 등록을 지운다
		KRS_LOG(Warning, "Asset load failed: {}", SourcePath);
		AssetPathMap.Remove(Key);
		return nullptr;
	}
	return *Loaded;
}

URenderAsset* UAssetManager::FindLoadedAsset(const FString& Path)
{
	URenderAsset** Found = Get().AssetMap.Find(FAssetUtils::GetAssetKey(Path));
	return Found ? *Found : nullptr;
}

EAssetType UAssetManager::GetUnloadedAssetType(const FString& Path)
{
	const FString Key = FAssetUtils::GetAssetKey(Path);
	if (Get().AssetMap.Contains(Key) || !Get().AssetPathMap.Contains(Key))
	{
		return EAssetType::None;
	}
	return GetAssetTypeFromPath(Key);
}

TArray<FString> UAssetManager::GetUnloadedAssetPaths(EAssetType Type)
{
	TArray<FString> Paths;
	for (const auto& [Key, Path] : Get().AssetPathMap)
	{
		if (GetAssetTypeFromPath(Key) == Type && !Get().AssetMap.Contains(Key))
		{
			Paths.Add(Path);
		}
	}
	return Paths;
}

void UAssetManager::CreateDefaultTextures() {
	// 텍스처가 지정되지 않은 머티리얼이 검게 나오지 않도록 하는 1x1 흰색 텍스처.
	// 셰이더에서 곱해도 결과가 변하지 않으므로 "텍스처 없음"의 기본값으로 쓴다.
	const uint32 WhitePixel{ 0xFFFFFFFF };

	FTextureDesc Desc{};
	Desc.Width = 1;
	Desc.Height = 1;
	Desc.Format = ETextureFormat::R8G8B8A8_UNorm;
	Desc.Usage = ETextureUsage::Immutable;
	Desc.BindFlags = ETextureBindFlags::ShaderResource;

	TUniquePtr<FTexture2D> Resource{ RenderCommand::CreateTexture2D(Desc, &WhitePixel) };
	if (!Resource) {
		return;
	}

	UTexture2D* WhiteTexture{ NewObject<UTexture2D>() };
	WhiteTexture->SetResource(std::move(Resource));
	WhiteTexture->SetPath("WhiteTexture");

	AssetMap["WhiteTexture"] = WhiteTexture;

	const FVector4 NormalPixel{ 0.5f, 0.5f, 1.0f, 1.0f };
	Desc.Format = ETextureFormat::R32G32B32A32_Float;
	TUniquePtr<FTexture2D> NormalResource{ RenderCommand::CreateTexture2D(Desc, &NormalPixel) };
	if (!NormalResource) {
		return;
	}

	UTexture2D* FlatNormalTexture{ NewObject<UTexture2D>() };
	FlatNormalTexture->SetResource(std::move(NormalResource));
	FlatNormalTexture->SetPath("FlatNormalTexture");
	AssetMap["FlatNormalTexture"] = FlatNormalTexture;
}

void UAssetManager::CreateDefaultMeshes()
{
	// 메시 데이터 업로드
	FStaticMeshData* CubeData = FGeometryGenerator::GetMeshData("Cube");
	RegisterAsset("Cube", CreateStaticMesh(CubeData));

	// 메시 파티클용 흰 큐브. 기본 Cube는 면마다 정점 색이 있어 파티클 색과 곱하면 노란색이 나오지 않는다
	if (CubeData)
	{
		FStaticMeshData WhiteCube = *CubeData;
		for (FVertex& Vertex : WhiteCube.Vertices)
		{
			Vertex.Color = FVector4(1.0f, 1.0f, 1.0f, 1.0f);
		}
		RegisterAsset("ParticleSparkCube", CreateStaticMesh(&WhiteCube));
	}

	FStaticMeshData* ConeData = FGeometryGenerator::GetMeshData("Cone");
	RegisterAsset("Cone", CreateStaticMesh(ConeData));

	FStaticMeshData* SphereData = FGeometryGenerator::GetMeshData("Sphere");
	RegisterAsset("Sphere", CreateStaticMesh(SphereData));

	FStaticMeshData* CylinderData = FGeometryGenerator::GetMeshData("Cylinder");
	RegisterAsset("Cylinder", CreateStaticMesh(CylinderData));

	FStaticMeshData* PlaneData = FGeometryGenerator::GetMeshData("Plane");
	RegisterAsset("Plane", CreateStaticMesh(PlaneData));

	FStaticMeshData* ArrowMeshData = FGeometryGenerator::GetMeshData("Arrow");
	RegisterAsset("Arrow", CreateStaticMesh(ArrowMeshData));

	FStaticMeshData* RingMeshData = FGeometryGenerator::GetMeshData("Ring");
	RegisterAsset("Ring", CreateStaticMesh(RingMeshData));

	FStaticMeshData* TorusData = FGeometryGenerator::GetMeshData("Torus");
	RegisterAsset("Torus", CreateStaticMesh(TorusData));

	FStaticMeshData* ScaleBarData = FGeometryGenerator::GetMeshData("ScaleBar");
	RegisterAsset("ScaleBar", CreateStaticMesh(ScaleBarData));

	FStaticMeshData* GizmoSphereData = FGeometryGenerator::GetMeshData("GizmoSphere");
	RegisterAsset("GizmoSphere", CreateStaticMesh(GizmoSphereData));

	const FBillboardVertex Vertices[] =
	{
		{ FVector(0.0f, -0.5f,  0.5f), FVector2(0.0f, 0.0f) },
		{ FVector(0.0f, 0.5f,  0.5f), FVector2(1.0f, 0.0f) },
		{ FVector(0.0f, 0.5f, -0.5f), FVector2(1.0f, 1.0f) },
		{ FVector(0.0f, -0.5f, -0.5f), FVector2(0.0f, 1.0f) }
	};

	const uint32 Indices[] =
	{
		0, 1, 2,
		0, 2, 3
	};

	UStaticMesh* Mesh = NewObject<UStaticMesh>();
	Mesh->VertexBuffer = RenderCommand::CreateStaticVertexBuffer(
		Vertices,
		sizeof(Vertices), sizeof(FBillboardVertex));
	Mesh->IndexBuffer = RenderCommand::CreateStaticIndexBuffer(
		Indices,
		ARRAYSIZE(Indices));

	FVertex Vertex{};
	Vertex.Position = FVector(0.0f, -0.5f, 0.5f);
	Mesh->MeshData.Vertices.Add(Vertex);
	Vertex.Position = FVector(0.0f, 0.5f, 0.5f);
	Mesh->MeshData.Vertices.Add(Vertex);
	Vertex.Position = FVector(0.0f, 0.5f, -0.5f);
	Mesh->MeshData.Vertices.Add(Vertex);
	Vertex.Position = FVector(0.0f, -0.5f, -0.5f);
	Mesh->MeshData.Vertices.Add(Vertex);

	Mesh->MeshData.Indices = { 0, 1, 2, 0, 2, 3, 0,2,1, 0,3,2 };

	Mesh->MeshData.AABB.Min = FVector(-0.001f, -0.5, -0.5);
	Mesh->MeshData.AABB.Max = FVector(0.001f, 0.5, 0.5);
	Mesh->RebuildRenderData();

	RegisterAsset("BillboardQuad", Mesh);
}

void UAssetManager::CreateDefaultMaterial() {
	UMaterial* DefaultMat{ NewObject<UMaterial>() };
	DefaultMat->Shader = FRenderResourceManager::GetShaderProgram("EngineResources/Shaders/UberLit.hlsl");
	DefaultMat->Textures.Add(GetAssetByPath<UTexture2D>("WhiteTexture"));
	DefaultMat->Textures.Add(GetAssetByPath<UTexture2D>("FlatNormalTexture"));
	DefaultMat->ParamLayout = EMaterialParamLayout::StaticMesh;
	DefaultMat->ParamBuffer = RenderCommand::CreateConstantBuffer(sizeof(FStaticMeshMaterialParams));
	RegisterAsset("DefaultMaterial", DefaultMat);

	// 씬 역직렬화 시 스타킹 마스터 머티리얼을 찾을 수 있도록 기본 에셋으로 등록한다
	if (UMaterial* StockingMaster = FStockingMaterialLibrary::GetOrCreateStockingMasterMaterial())
	{
		RegisterAsset("Engine/Materials/M_StockingMaster_Lit", StockingMaster);
	}
}

void UAssetManager::CreateBillboardMaterial()
{
	UMaterial* BillboardMat = NewObject<UMaterial>();
	BillboardMat->PSOType = EPSOType::Billboard;
	BillboardMat->Textures.Add(GetAssetByPath<UTexture2D>("WhiteTexture"));
	BillboardMat->BlendState = EBlendState::AlphaBlend;
	BillboardMat->DepthStencilState = EDepthStencilState::ReadOnly;
	BillboardMat->SamplerState = ESamplerState::LinearClamp;
	BillboardMat->ParamLayout = EMaterialParamLayout::Billboard;
	RegisterAsset("BillboardMaterial", BillboardMat);
}

void UAssetManager::CreateParticleMaterial()
{
	auto MakeSpriteMaterial = [this](EPSOType PSOType, EBlendState Blend, const char* Name)
		{
			UMaterial* Mat = NewObject<UMaterial>();
			Mat->PSOType = PSOType;
			Mat->Shader = FRenderResourceManager::GetShaderProgram("EngineResources/Shaders/ParticleSpriteShader.hlsl");
			Mat->Textures.Add(GetAssetByPath<UTexture2D>("Engine/SubUV/StarParticle.png"));
			Mat->BlendState = Blend;
			Mat->DepthStencilState = EDepthStencilState::ReadOnly;
			Mat->SamplerState = ESamplerState::LinearClamp;
			RegisterAsset(Name, Mat);
		};
	MakeSpriteMaterial(EPSOType::Particle_Additive, EBlendState::ParticleAdditive, "ParticleSpriteAdditive");
	MakeSpriteMaterial(EPSOType::Particle_AlphaBlend, EBlendState::AlphaBlend, "ParticleSpriteAlphaBlend");

	// 연기용. CloudAtlas는 8x8 아틀라스라 에미터에서 SubUV 8x8을 함께 설정해야 한다.
	// 콘텐츠 스캔 뒤에 불리므로 프로젝트에 텍스처가 있을 때만 등록한다
	if (UTexture2D* CloudAtlas = GetAssetByPath<UTexture2D>("Textures/Particles/CloudAtlas.png"))
	{
		UMaterial* SmokeMat = NewObject<UMaterial>();
		SmokeMat->PSOType = EPSOType::Particle_AlphaBlend;
		SmokeMat->Shader = FRenderResourceManager::GetShaderProgram("EngineResources/Shaders/ParticleSpriteShader.hlsl");
		SmokeMat->Textures.Add(CloudAtlas);
		SmokeMat->BlendState = EBlendState::AlphaBlend;
		SmokeMat->DepthStencilState = EDepthStencilState::ReadOnly;
		SmokeMat->SamplerState = ESamplerState::LinearClamp;
		RegisterAsset("ParticleSmokeCloud", SmokeMat);
	}

	// 빔, 리본용. U(길이 방향)는 반복, V(폭 방향)는 가장자리가 번지지 않게 Clamp
	// 셰이더는 스프라이트 것을 두어야 Trail 정점 타입일 때 Trail 셰이더로 바뀐다
	UMaterial* BeamMat = NewObject<UMaterial>();
	BeamMat->PSOType = EPSOType::Particle_Additive;
	BeamMat->Shader = FRenderResourceManager::GetShaderProgram("EngineResources/Shaders/ParticleSpriteShader.hlsl");
	BeamMat->Textures.Add(GetAssetByPath<UTexture2D>("Engine/Textures/BeamGlow.png"));
	BeamMat->BlendState = EBlendState::ParticleAdditive;
	BeamMat->DepthStencilState = EDepthStencilState::ReadOnly;
	BeamMat->SamplerState = ESamplerState::LinearWrapUClampV;
	RegisterAsset("ParticleBeamAdditive", BeamMat);

	// 메시 에미터의 머티리얼 오버라이드용. 스프라이트 셰이더는 메시 파티클 정점을 받지 못한다
	auto MakeMeshMaterial = [this](EPSOType PSOType, EBlendState Blend, const char* Name)
		{
			UMaterial* Mat = NewObject<UMaterial>();
			Mat->PSOType = PSOType;
			Mat->Shader = FRenderResourceManager::GetShaderProgram("EngineResources/Shaders/StaticMeshTranslucentShader.hlsl");
			Mat->Textures.Add(GetAssetByPath<UTexture2D>("WhiteTexture"));
			Mat->BlendState = Blend;
			Mat->DepthStencilState = EDepthStencilState::ReadOnly;
			Mat->SamplerState = ESamplerState::LinearClamp;
			Mat->ParamLayout = EMaterialParamLayout::StaticMesh;
			Mat->ParamBuffer = RenderCommand::CreateConstantBuffer(sizeof(FStaticMeshMaterialParams));
			RegisterAsset(Name, Mat);
		};
	MakeMeshMaterial(EPSOType::StaticMesh_Translucent, EBlendState::AlphaBlend, "ParticleMeshTranslucent");
	MakeMeshMaterial(EPSOType::StaticMesh_Additive, EBlendState::ParticleAdditive, "ParticleMeshAdditive");
}

void UAssetManager::CreateEditorArrowMaterial() {
	UMaterial* BaseMaterial{ GetAssetByPath<UMaterial>("DefaultMaterial") };
	UMaterial* ArrowMaterial{ UMaterial::CreateInstance(BaseMaterial) };
	ArrowMaterial->BaseColor = FVector4{ 1.0f, 1.0f, 1.0f, 1.0f };
	RegisterAsset("EditorArrowMaterial", ArrowMaterial);
}

void UAssetManager::Shutdown()
{
	Get().PhysicsAssetMap.Empty();
	Get().AssetMap.Empty();
	Get().SoundMap.Empty();
	Get().AnimSequenceMap.Empty();
	Get().BlendSpaceMap.Empty();
	for (auto& Pair : Get().ParticleSystemMap)
	{
		delete Pair.second;
	}
	Get().ParticleSystemMap.Empty();
}


void UAssetManager::RegisterAsset(const FString& Key, URenderAsset* Asset)
{
	// 경로 표기는 씬 저장 형식이 바뀌지 않도록 받은 그대로 둔다
	Asset->SetPath(Key);
	AssetMap[FAssetUtils::GetAssetKey(Key)] = Asset;
}

UTexture2D* UAssetManager::LoadTexture(const FString& InPath, bool GenerateMips) {
	const FString Key = FAssetUtils::GetAssetKey(InPath);
	if (URenderAsset** Found{ AssetMap.Find(Key) }) {
		return Cast<UTexture2D>(*Found);
	}

	// 옛 경로("Content/Game/Icons/Decal.png")로 불렸는데 프로젝트에는 없고 엔진 콘텐츠에 있으면 그쪽을 쓴다.
	if (!AssetPathMap.Contains(Key))
	{
		const FString EngineKey = MakeEngineFallbackKey(Key);
		if (!EngineKey.empty() && AssetPathMap.Contains(EngineKey))
		{
			return Cast<UTexture2D>(FindOrLoadAsset(EngineKey));
		}
	}

	FLoadStageTimer DecodeTimer{ "Texture/Decode" };
	const FImageData Data = ImageLoader::LoadAuto(FAssetUtils::ResolveAssetFilePath(InPath));
	DecodeTimer.Stop();
	if (!Data.IsValid())
	{
		return nullptr;
	}

	const bool bUseDDSLevels = Data.MipRowPitches.Num() > 0;
	// BC 압축 DDS는 렌더 타깃으로 바인딩할 수 없으므로 GPU mip 생성 대상에서 제외한다.
	const bool bGenerateGPUmips = GenerateMips && !bUseDDSLevels && Data.BytesPerPixel != 0;

	FTextureDesc Desc{};
	Desc.Width = Data.Width;
	Desc.Height = Data.Height;
	Desc.MipLevels = bGenerateGPUmips ? 0 : Data.MipLevels;
	Desc.Usage = bGenerateGPUmips ? ETextureUsage::Default : ETextureUsage::Immutable;
	Desc.BindFlags = ETextureBindFlags::ShaderResource |
		(bGenerateGPUmips ? ETextureBindFlags::RenderTarget : ETextureBindFlags::None);
	Desc.bGenerateMips = bGenerateGPUmips;

	FLoadStageTimer CreateTimer{ "Texture/CreateTexture2D" };
	TUniquePtr<FTexture2D> Resource = RenderCommand::CreateTexture2D(Desc, Data);
	CreateTimer.Stop();

	if (!Resource || !Resource->GetSRV()) {
		return nullptr;
	}

	UTexture2D* Asset{ NewObject<UTexture2D>() };
	Asset->SetResource(std::move(Resource));
	RegisterAsset(InPath, Asset);

	return Asset;
}

UFont* UAssetManager::LoadFontAtlas(const FString& JsonPath, const FString& AtlasTexturePath)
{
	TUniquePtr<UFont> Font(NewObject<UFont>());

	if (!Font->LoadFontAtlasJson(FPaths::ResolveContentPath(JsonPath)))
	{
		return nullptr;
	}

	Font->AtlasTexture = LoadTexture(AtlasTexturePath, false);
	if (!Font->AtlasTexture)
	{
		return nullptr;
	}

	RegisterAsset(JsonPath, Font.get());
	return Font.release();
}

USoundWave* UAssetManager::LoadSound(const FString& Path)
{
	const FString Key = FAssetUtils::GetAssetKey(Path);
	if (Key.empty()) return nullptr;
	if (USoundWave** Found = SoundMap.Find(Key)) return *Found;

	TUniquePtr<USoundWave> Wave(NewObject<USoundWave>());
	if (!Wave->LoadFromFile(FAssetUtils::ResolveAssetFilePath(Path)))
	{
		return nullptr;
	}
	SoundMap[Key] = Wave.get();
	return Wave.release();
}

UStaticMesh* UAssetManager::LoadObjStaticMesh(const FString& Path)
{
	const FString Key = FAssetUtils::GetAssetKey(Path);
	// GetAssetByPath는 여기로 다시 들어오므로 쓰지 않는다
	if (URenderAsset* Cached = FindLoadedAsset(Key))
	{
		return Cast<UStaticMesh>(Cached);
	}

	FLoadStageTimer ReadTimer{ "Mesh/ReadBakedOrCook" };
	TUniquePtr<FStaticMeshData> Data = FObjImporter::LoadStaticMeshData(FAssetUtils::ResolveAssetFilePath(Path));
	ReadTimer.Stop();
	UStaticMesh* Mesh = CreateStaticMesh(Data.get());   // Data가 nullptr이면 nullptr 반환
	if (!Mesh)
	{
		return nullptr;
	}

	Get().RegisterAsset(Key, Mesh);
	return Mesh;
}

URenderAsset* UAssetManager::LoadFbxMesh(const FString& Path, FString& OutError)
{
	OutError.clear();
	const FString Key = FAssetUtils::GetAssetKey(Path);
	// GetAssetByPath는 여기로 다시 들어오므로 쓰지 않는다
	if (URenderAsset* Cached = FindLoadedAsset(Key)) return Cached;
	FFbxBakedMesh Data;
	if (!FFbxMeshBake::IsBakedPath(Path) || !FFbxMeshBake::Read(FAssetUtils::ResolveAssetFilePath(Path), Data, OutError))
	{
		if (OutError.empty()) OutError = "FBX binary: expected .fbx.bin: " + Path;
		return nullptr;
	}
	URenderAsset* Asset = nullptr;
	if (Data.Kind == EFbxMeshKind::Static) Asset = CreateStaticMesh(&Data.StaticMesh);
	else
	{
		auto* Mesh = NewObject<USkeletalMesh>();
		if (!Mesh->Build(std::move(Data.SkeletalMesh), OutError)) return nullptr;
		Asset = Mesh;
	}
	if (!Asset) { OutError = "FBX binary: mesh resource creation failed: " + Path; return nullptr; }
	Get().RegisterAsset(Key, Asset);
	if (USkeletalMesh* SkelMesh = Cast<USkeletalMesh>(Asset))
	{
		const FString ResolvedFile = FAssetUtils::ResolveAssetFilePath(Key);
		if (!ResolvedFile.empty())
		{
			SkelMesh->LoadSocketsFromFile(ResolvedFile + ".sockets.json");

			FString PhysicsError;
			if (!SkelMesh->LoadPhysicsAssetReference(PhysicsError))
				KRS_LOG(Warning, "[PhysicsAsset] {}", PhysicsError);
		}
	}
	return Asset;
}

UPhysicsAsset* UAssetManager::LoadPhysicsAsset(const FString& Path, FString& OutError)
{
	OutError.clear();
	const FString Key = FAssetUtils::GetAssetKey(Path);
	const FString FilePath = FAssetUtils::ResolveAssetFilePath(Key);
	std::error_code Error;
	const fs::path FileName = FPaths::FromUtf8(FilePath);
	if (Key.empty() || !PhysicsAssetFile::IsPhysicsAssetPath(Key) || !fs::is_regular_file(FileName, Error))
	{
		OutError = "The Physics Asset file is missing or invalid: " + Path;
		return nullptr;
	}
	const fs::file_time_type WriteTime = fs::last_write_time(FileName, Error);
	if (Error) { OutError = "Cannot read the Physics Asset file timestamp: " + Path; return nullptr; }
	if (const FLoadedPhysicsAsset* Cached = Get().PhysicsAssetMap.Find(Key))
		if (Cached->LastWriteTime == WriteTime) return Cached->Asset;

	std::ifstream File(FileName, std::ios::binary | std::ios::ate);
	if (!File) { OutError = "Cannot open the Physics Asset: " + Path; return nullptr; }
	const std::streamsize Size = File.tellg();
	if (Size <= 0 || Size > std::numeric_limits<int32>::max())
	{
		OutError = "The Physics Asset file size is invalid: " + Path;
		return nullptr;
	}
	TArray<uint8> Bytes;
	Bytes.SetNum(static_cast<int32>(Size));
	File.seekg(0);
	if (!File.read(reinterpret_cast<char*>(Bytes.GetData()), Size))
	{
		OutError = "Cannot read the Physics Asset: " + Path;
		return nullptr;
	}
	TUniquePtr<UPhysicsAsset> Asset(NewObject<UPhysicsAsset>());
	if (!Asset) { OutError = "Cannot allocate the Physics Asset."; return nullptr; }
	if (!FJsonArchive::LoadObjectFromBytes(Asset.get(), Bytes))
	{
		Asset->ClearBodies();
		OutError = "Cannot deserialize the Physics Asset: " + Path;
		return nullptr;
	}
	Asset->SetAssetPath(Key);
	// 이전 버전은 이미 생성된 바디가 참조할 수 있어 UObject 일괄 정리 때까지 유지한다.
	Get().PhysicsAssetMap[Key] = { Asset.get(), WriteTime };
	return Asset.release();
}

void UAssetManager::RegisterPhysicsAsset(const FString& Path, UPhysicsAsset* Asset)
{
	const FString Key = FAssetUtils::GetAssetKey(Path);
	if (Key.empty() || !Asset)
	{
		return;
	}
	const FString FilePath = FAssetUtils::ResolveAssetFilePath(Key);
	std::error_code Error;
	const fs::file_time_type WriteTime = FilePath.empty()
		? fs::file_time_type{}
		: fs::last_write_time(FPaths::FromUtf8(FilePath), Error);
	Asset->SetAssetPath(Key);
	Get().PhysicsAssetMap[Key] = { Asset, WriteTime };
}

UAnimSequence* UAssetManager::LoadAnimSequence(const FString& Path)
{
	const FString Key = FAssetUtils::GetAssetKey(Path);
	if (UAnimSequence** Found = Get().AnimSequenceMap.Find(Key))
	{
		return *Found;
	}

	const FString FilePath = FAssetUtils::ResolveAssetFilePath(Path);
	UAnimSequence* Sequence = NewObject<UAnimSequence>();
	FString Error;
	if (!AnimSequenceFile::Read(std::filesystem::path(FilePath.empty() ? Path : FilePath), *Sequence, Error))
	{
		KRS_LOG(Warning, "Failed to load animation sequence: {} ({})", Path, Error);
		delete Sequence;    // 캐시에 넣지 않은 객체는 아무도 지우지 않으므로 여기서 지운다
		return nullptr;
	}

	Get().AnimSequenceMap[Key] = Sequence;
	return Sequence;
}

bool UAssetManager::ReorientStaticMesh(UStaticMesh& Mesh, const std::function<FVector(const FVector&)>& Rotate)
{
	if (Mesh.MeshData.Vertices.IsEmpty())
		return false;

	FStaticMeshData& Data = Mesh.MeshData;
	FBox Bounds{ FVector(FLT_MAX, FLT_MAX, FLT_MAX), FVector(-FLT_MAX, -FLT_MAX, -FLT_MAX) };
	for (FVertex& Vertex : Data.Vertices)
	{
		Vertex.Position = Rotate(Vertex.Position);
		Vertex.Normal = Rotate(Vertex.Normal);
		Bounds.Min = FVector(std::min(Bounds.Min.X, Vertex.Position.X), std::min(Bounds.Min.Y, Vertex.Position.Y), std::min(Bounds.Min.Z, Vertex.Position.Z));
		Bounds.Max = FVector(std::max(Bounds.Max.X, Vertex.Position.X), std::max(Bounds.Max.Y, Vertex.Position.Y), std::max(Bounds.Max.Z, Vertex.Position.Z));
	}
	Data.AABB = Bounds;
	Data.BuildTangentSpace();

	// 인덱스는 그대로다 (회전은 삼각형 감기 방향을 바꾸지 않는다). 정점 버퍼만 다시 올린다.
	TUniquePtr<FVertexBuffer> VB = RenderCommand::CreateStaticVertexBuffer(Data.Vertices.GetData(),
		sizeof(FVertex) * static_cast<uint32>(Data.Vertices.Num()), sizeof(FVertex));
	if (!VB)
		return false;
	Mesh.VertexBuffer = std::move(VB);

	Data.BuildTriangleBVH();   // 피킹용 삼각형 BVH도 새 좌표로
	Data.BuildOBBTree();
	return true;
}

void UAssetManager::ForEachObjStaticMesh(const std::function<void(const FString& Key, UStaticMesh& Mesh)>& Func)
{
	for (const FString& Path : GetUnloadedAssetPaths(EAssetType::StaticMesh))
	{
		if (!FFbxMeshBake::IsBakedPath(Path))
		{
			GetAssetByPath<UStaticMesh>(Path);
		}
	}

	for (auto& [Key, Asset] : Get().AssetMap)
	{
		if (!Asset || !Asset->IsA<UStaticMesh>() || !fs::path(Key).extension().string().ends_with(".obj"))
			continue;
		Func(Key, *Cast<UStaticMesh>(Asset));
	}
}

namespace
{
	template<typename TSlot>
	bool BuildMaterialsFromSlotsImpl(const TArray<TSlot>& Slots, TArray<UMaterialInterface*>& OutMaterials)
	{
		// MaterialSlots를 실제 UMaterial로 변환
		UMaterial* DefaultMaterial = UAssetManager::GetAssetByPath<UMaterial>("DefaultMaterial");
		if (!DefaultMaterial)
		{
			return false;
		}

		for (const TSlot& Slot : Slots)
		{
			UMaterial* Material = UMaterial::CreateInstance(DefaultMaterial);
			if (!Material)
			{
				return false;
			}
			Material->BaseColor = Slot.BaseColor;
			Material->ApplyImportedProperties(Slot);
			// 임포트 메쉬의 UV는 0~1 밖의 타일을 쓰는 경우가 많아 반복 샘플링을 기본으로 한다.
			Material->SamplerState = ESamplerState::LinearWrap;
			if (!Slot.bAlphaMasked && Material->BaseColor.W < 1.0f)
			{
				Material->SetBlendState(EBlendState::AlphaBlend);
			}

			if (!Slot.DiffuseTexturePath.empty())
			{
				UTexture2D* Texture = UAssetManager::Get().LoadTexture(Slot.DiffuseTexturePath);

				if (Texture)
				{
					Material->Textures[0] = Texture;
				}
			}

			if (!Slot.NormalTexturePath.empty())
			{
				if (UTexture2D* NormalTexture = UAssetManager::Get().LoadTexture(Slot.NormalTexturePath))
				{
					Material->Textures[1] = NormalTexture;
				}
			}

			OutMaterials.Add(Material);
		}
		return true;
	}

}

bool UAssetManager::BuildMaterialsFromSlots(const TArray<FStaticMaterialSlot>& Slots, TArray<UMaterialInterface*>& OutMaterials)
{
	return BuildMaterialsFromSlotsImpl(Slots, OutMaterials);
}

bool UAssetManager::BuildMaterialsFromSlots(const TArray<FSkeletalMaterialSlot>& Slots, TArray<UMaterialInterface*>& OutMaterials)
{
	return BuildMaterialsFromSlotsImpl(Slots, OutMaterials);
}

void UAssetManager::RegisterMaterialAsset(const FString& AssetPath, UMaterialInterface* Material)
{
	if (!Material)
	{
		return;
	}

	const FString Key = FAssetUtils::GetAssetKey(AssetPath);
	Material->SetPath(Key);
	Get().RegisterAsset(Key, Material);

	const FString ResolvedFile = FAssetUtils::ResolveAssetFilePath(Key);
	if (!ResolvedFile.empty())
	{
		Get().AssetPathMap.Add(Key, ResolvedFile);
	}
}

UMaterialInterface* UAssetManager::LoadMaterialAsset(const FString& AssetPath)
{
	if (AssetPath.empty())
	{
		return nullptr;
	}

	// GetAssetByPath는 FindOrLoadAsset에서 다시 여기로 들어오므로 쓰지 않는다
	if (UMaterialInterface* Cached = Cast<UMaterialInterface>(FindLoadedAsset(AssetPath)))
	{
		return Cached;
	}

	const FString Key = FAssetUtils::GetAssetKey(AssetPath);
	if (fs::path(FPaths::FromUtf8(Key)).extension() != ".material")
	{
		return nullptr;
	}

	static std::set<FString> LoadingMaterials;
	if (!LoadingMaterials.insert(Key).second)
	{
		return nullptr;
	}

	FString FilePath = FAssetUtils::ResolveAssetFilePath(Key);
	if (FilePath.empty())
	{
		FilePath = FPaths::ToUtf8(FPaths::ProjectContentDir() / FPaths::FromUtf8(Key));
	}

	std::ifstream Input(FPaths::FromUtf8(FilePath));
	if (!Input)
	{
		KRS_LOG(Warning, "Material not found: {}", AssetPath);
		LoadingMaterials.erase(Key);
		return nullptr;
	}

	const json Root = json::parse(Input, nullptr, false);
	if (!Root.is_object() || !Root.contains("Class") || !Root["Class"].is_string() || !Root.contains("Object") || !Root["Object"].is_object())
	{
		KRS_LOG(Warning, "Invalid Material asset structure: {}", Key);
		LoadingMaterials.erase(Key);
		return nullptr;
	}

	const FString ClassName = Root["Class"].get<FString>();
	UMaterialInterface* Material = nullptr;
	if (ClassName == UMaterialInstanceConstant::StaticClass()->Name || ClassName == "UMaterialInstance")
	{
		Material = NewObject<UMaterialInstanceConstant>();
	}
	else if (ClassName == UMaterial::StaticClass()->Name)
	{
		Material = NewObject<UMaterial>();
	}

	const FString LegacyClassName = ClassName == "UMaterialInstance" ? "UMaterialInstance" : "";
	if (!Material || !FJsonArchive::LoadObject(Material, FilePath, LegacyClassName))
	{
		if (Material)
		{
			delete Material;
		}
		LoadingMaterials.erase(Key);
		return nullptr;
	}

	if (UMaterial* BaseMat = Cast<UMaterial>(Material))
	{
		BaseMat->ParamLayout = EMaterialParamLayout::StaticMesh;
		BaseMat->Shader = FRenderResourceManager::GetShaderProgram("EngineResources/Shaders/UberLit.hlsl");
		BaseMat->ParamBuffer = RenderCommand::CreateConstantBuffer(sizeof(FStaticMeshMaterialParams));
	}

	Material->SetPath(Key);
	Get().RegisterAsset(Key, Material);
	LoadingMaterials.erase(Key);
	return Material;
}

bool UAssetManager::SaveMaterialAsset(const FString& AssetPath, const UMaterialInterface& Material)
{
	const FString Key = FAssetUtils::GetAssetKey(AssetPath);
	if (Key.empty() || fs::path(FPaths::FromUtf8(Key)).extension() != ".material")
	{
		return false;
	}

	const fs::path FilePath = FPaths::ProjectContentDir() / FPaths::FromUtf8(Key);
	std::error_code Error;
	fs::create_directories(FilePath.parent_path(), Error);
	if (Error)
	{
		return false;
	}

	UMaterialInterface* MutableMaterial = const_cast<UMaterialInterface*>(&Material);
	if (!FJsonArchive::SaveObject(MutableMaterial, FPaths::ToUtf8(FilePath)))
	{
		return false;
	}

	MutableMaterial->SetPath(Key);
	RegisterMaterialAsset(Key, MutableMaterial);
	return true;
}

UBlendSpace1D* UAssetManager::LoadBlendSpace(const FString& Path)
{
	const FString Key = FAssetUtils::GetAssetKey(Path);
	if (UBlendSpace1D** Found = Get().BlendSpaceMap.Find(Key))
	{
		return *Found;
	}

	const FString FilePath = FAssetUtils::ResolveAssetFilePath(Path);
	UBlendSpace1D* BlendSpace = NewObject<UBlendSpace1D>();

	FString Error;
	if (!BlendSpaceFile::Read(std::filesystem::path(FilePath.empty() ? Path : FilePath), *BlendSpace, Error))
	{
		KRS_LOG(Warning, "Failed to load BlendSpace: {} ({})", Path, Error);
		delete BlendSpace;
		return nullptr;
	}

	Get().BlendSpaceMap[Key] = BlendSpace;
	return BlendSpace;
}

UBlendSpace2D* UAssetManager::LoadBlendSpace2D(const FString& Path)
{
	const FString Key = FAssetUtils::GetAssetKey(Path);
	if (UBlendSpace2D** Found = Get().BlendSpace2DMap.Find(Key))
	{
		return *Found;
	}

	const FString FilePath = FAssetUtils::ResolveAssetFilePath(Path);
	UBlendSpace2D* BlendSpace = NewObject<UBlendSpace2D>();

	FString Error;
	if (!BlendSpaceFile::Read2D(std::filesystem::path(FilePath.empty() ? Path : FilePath), *BlendSpace, Error))
	{
		KRS_LOG(Warning, "Failed to load BlendSpace2D: {} ({})", Path, Error);
		delete BlendSpace;
		return nullptr;
	}

	Get().BlendSpace2DMap[Key] = BlendSpace;
	return BlendSpace;
}

UParticleSystem* UAssetManager::LoadParticleSystem(const FString& Path)
{
	const FString Key = FAssetUtils::GetAssetKey(Path);
	if (UParticleSystem** Found = Get().ParticleSystemMap.Find(Key))
	{
		return *Found;
	}

	const FString FilePath = FAssetUtils::ResolveAssetFilePath(Path);
	UParticleSystem* ParticleSystem = NewObject<UParticleSystem>();

	FString Error;
	if (!ParticleSystemFile::Read(std::filesystem::path(FilePath.empty() ? Path : FilePath), *ParticleSystem, Error))
	{
		KRS_LOG(Warning, "Failed to load ParticleSystem: {} ({})", Path, Error);
		delete ParticleSystem;
		return nullptr;
	}

	Get().ParticleSystemMap[Key] = ParticleSystem;
	return ParticleSystem;
}

FLODGenerateResult UAssetManager::GenerateStaticMeshLODs(UStaticMesh& Mesh, const FLODGenerateRequest& Request)
{
	TArray<FStaticMeshData> Generated;
	FLODGenerateResult Result = FStaticMeshLODGenerator::Generate(Mesh.GetMeshData(), Generated);
	if (!Result.bSuccess)
	{
		return Result;
	}

	if (!(Request.ScreenThresholds[0] > Request.ScreenThresholds[1] &&
		Request.ScreenThresholds[1] > Request.ScreenThresholds[2] &&
		Request.ScreenThresholds[2] > 0.0f))
	{
		Result.bSuccess = false;
		Result.FailureReason = "Screen thresholds must descend";
		return Result;
	}

	TArray<FStaticMeshLODResource> NewResources;
	for (FStaticMeshData& Data : Generated)
	{
		FStaticMeshLODResource Resource;
		Resource.Data = std::move(Data);
		Resource.Data.BuildTangentSpace();
		Resource.Data.OptimizeTriangleOrderForVertexCache();
		Resource.VertexBuffer = RenderCommand::CreateStaticVertexBuffer(
			Resource.Data.Vertices.GetData(),
			sizeof(FVertex) * static_cast<uint32>(Resource.Data.Vertices.Num()),
			sizeof(FVertex));
		Resource.IndexBuffer = RenderCommand::CreateStaticIndexBuffer(
			Resource.Data.Indices.GetData(),
			static_cast<uint32>(Resource.Data.Indices.Num()));
		if (!Resource.VertexBuffer || !Resource.IndexBuffer)
		{
			Result.bSuccess = false;
			Result.FailureReason = "LOD GPU buffer creation failed";
			return Result;
		}
		NewResources.Add(std::move(Resource));
	}

	Mesh.AdditionalLODs = std::move(NewResources);
	Mesh.ScreenThresholds = Request.ScreenThresholds;
	Mesh.RebuildRenderData();
	return Result;
}

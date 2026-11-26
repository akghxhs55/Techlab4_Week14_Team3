#pragma once

#include "Render/MeshBatch.h"
#include "Render/ObjectConstants.h"
#include "Texture2D.h"
#include "Text/Font.h"
#include "SceneView.h"
#include "Engine/LightSceneProxy.h"
#include "Render/Buffer.h"
#include "Render/Occlusion/GPUOcclusion.h"
#include "MeshPassProcessor.h"
#include <array>
#include <span>

class FHeightFogRenderer;
class FFXAARenderer;
class FDepthOfFieldRenderer;
class FCameraOverlayRenderer;

class FTextRenderer;
class FSkyboxRenderer;
class FProjectedDecalRenderer;

class FRenderer
{
public:
	// 전방 선언한 패스 객체를 TUniquePtr로 소유하므로 정의는 cpp에 둔다.
	FRenderer();
	~FRenderer();

	bool Init();

	void UploadViewConstants(const FMatrix& ViewProjection, const FMatrix& ViewMatrix,
		const FMatrix& ProjectionMatrix, float TotalTime = 0.0f);

	void SubmitVisibleCommands(std::span<const FVisibleMeshDrawCommand> Commands,
		const FStructuredBuffer* GPUSceneBuffer = nullptr, uint32* OutDrawCalls = nullptr,
		uint32* OutParticleDrawCalls = nullptr);

	void InitScenePasses();

	// FSceneRenderer::Render가 쓰는 상주 패스 객체. UE에서는 렌더러 모듈의 글로벌 자원에 해당한다.
	FHeightFogRenderer* GetHeightFog() const { return HeightFog.get(); }
	FFXAARenderer* GetFXAA() const { return FXAA.get(); }
	FDepthOfFieldRenderer* GetDepthOfField() const { return DepthOfField.get(); }
	FCameraOverlayRenderer* GetCameraOverlay() const { return CameraOverlay.get(); }
	FTextRenderer* GetSceneText() const { return SceneText.get(); }
	FSkyboxRenderer* GetSkybox() const { return Skybox.get(); }
	FSkyboxRenderer* GetPreviewSkybox() const { return PreviewSkybox.get(); }
	FProjectedDecalRenderer* GetProjectedDecal() const { return ProjectedDecal.get(); }

	FConstantBuffer* GetToneMapConstants() const { return ToneMapConstants.get(); }
	FConstantBuffer* GetDecalConstants() const { return DecalConstants.get(); }
	FGPUOcclusion& GetGPUOcclusion() { return GPUOcclusion; }

private:
	struct FDrawRun
	{
		const FVisibleMeshDrawCommand* Visible = nullptr;
		uint32 StartInstance = 0;
		uint32 InstanceCount = 0;
		bool bGPUSceneInstanced = false;
	};

	void EnsureInstanceIdCapacity(uint32 Count);
	void UnbindUnusedResources(const TArray<FMeshDrawShaderResource>& NextResources,
		TArray<FMeshDrawShaderResource>& InOutBoundResources);

	static constexpr uint32 MaxCommandSlotCBs = 5;
	std::array<TUniquePtr<FConstantBuffer>, MaxCommandSlotCBs> CommandSlotCBs;
	TUniquePtr<FConstantBuffer> ViewCB;
	TUniquePtr<FConstantBuffer> DrawBatchCB;
	TUniquePtr<FConstantBuffer> ToneMapConstants;
	TUniquePtr<FConstantBuffer> DecalConstants;
	TUniquePtr<FStructuredBuffer> InstanceIdBuffer;
	uint32 InstanceIdCapacity = 0;

	TArray<uint32> ScratchInstanceIds;
	TArray<FDrawRun> ScratchDrawRuns;

	TUniquePtr<FHeightFogRenderer> HeightFog;
	TUniquePtr<FFXAARenderer> FXAA;
	TUniquePtr<FDepthOfFieldRenderer> DepthOfField;
	TUniquePtr<FCameraOverlayRenderer> CameraOverlay;
	TUniquePtr<FTextRenderer> SceneText;
	TUniquePtr<FSkyboxRenderer> Skybox;
	TUniquePtr<FSkyboxRenderer> PreviewSkybox;
	TUniquePtr<FProjectedDecalRenderer> ProjectedDecal;
	FGPUOcclusion GPUOcclusion;
};

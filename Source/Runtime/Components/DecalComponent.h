#pragma once

#include "SceneComponent.h"
#include "Render/Texture2D.h"
#include "Render/Decal/ProjectedDecal.h"
#include "Render/Material.h"
#include "Math/Frustum.h"
#include "DecalComponent.generated.h"

UCLASS()
class UDecalComponent : public USceneComponent
{
	GENERATED_BODY()
public:
	void TickComponent(float DeltaTime) override;
	void UpdateInEditor() { Fade = 1.0f; }

	UDecalComponent() { PrimaryComponentTick.bCanEverTick = true; }
	
	UTexture2D* GetDecalTexture() const { return DecalTexture; }
	void SetDecalTexture(UTexture2D* InTexture) { DecalTexture = InTexture; }

	bool IsDecalEnabled() const { return bDecalEnabled; }
	void SetDecalEnabled(bool bInEnabled) { bDecalEnabled = bInEnabled; }

	UMaterialInterface* GetDecalMaterial() const { return DecalMaterial; }
	void SetDecalMaterial(UMaterialInterface* InMaterial) { DecalMaterial = InMaterial; }

	bool IsUnlit() const { return bUnlit; }
	void SetUnlit(bool bInUnlit) { bUnlit = bInUnlit; }

	float GetNormalStrength() const { return NormalStrength; }
	void SetNormalStrength(float InStrength) { NormalStrength = InStrength; }

	float GetNormalOpacity() const { return NormalOpacity; }
	void SetNormalOpacity(float InOpacity) { NormalOpacity = InOpacity; }

	// 투영 데칼 머티리얼 기반 DBuffer 렌더 데이터 생성
	bool TryGetRenderData(FProjectedDecalRenderData& OutRenderData) const;

	using Super::Serialize;
	void Serialize(FStructuredArchive::FRecord Record) override;
	
	FVector GetExtent() const { return Extent; }
	void SetExtent(FVector InExtent) { Extent = InExtent; }

	int32 GetSortOrder() const { return SortOrder; }
	void SetSortOrder(int32 InSortOrder) { SortOrder = InSortOrder; }

	void SetDecalViewProj();
	FMatrix GetDecalVP() { return DecalViewProj; }

	float GetFade() const { return Fade; }

	FAABB GetWorldAABB() const;
	FOBB GetWorldOBB() const;

	void GetWorldCorners(FVector OutCorners[8]) const;
	void DrawDebug(class FLineBatcher* LineBatcher) const;

	bool IsAdditive() const { return bAdditive; }

	void SetPerspective(bool bInPerspective) { bPerspective = bInPerspective; }
	void SetAdditive(bool bInAdditive) { bAdditive = bInAdditive; }
	void SetFadeTimes(float InDelay, float InFadeIn, float InVisible, float InFadeOut)
	{
		Delay = InDelay; FadeIn = InFadeIn; Visible = InVisible; FadeOut = InFadeOut;
	}

protected:
	virtual bool ShouldCreateRenderState() const override
	{
		return true;
	}

	virtual void CreateRenderState() override;
	virtual void DestroyRenderState() override;

private:
	float Ease(float t) const;


private:
	UPROPERTY(EditAnywhere) UTexture2D* DecalTexture = nullptr;
	UPROPERTY(EditAnywhere) FVector Extent = FVector(50, 50, 50);
	UPROPERTY(EditAnywhere) int32 SortOrder = 0;

	UPROPERTY(EditAnywhere) bool bPerspective = false;
	UPROPERTY(EditAnywhere) bool bAdditive = false;

	FMatrix DecalViewProj;

	float Fade = 0.0f;
	UPROPERTY(EditAnywhere) float Delay = 1.0f;
	UPROPERTY(EditAnywhere) float FadeIn = 1.0f;
	UPROPERTY(EditAnywhere) float Visible = 1.0f;
	UPROPERTY(EditAnywhere) float FadeOut = 1.0f;
	float CurrentTime = 0.0f;

	UPROPERTY(EditAnywhere) bool bDecalEnabled = true;
	UMaterialInterface* DecalMaterial = nullptr;
	UPROPERTY(EditAnywhere) bool bUnlit = false;
	UPROPERTY(EditAnywhere) float NormalStrength = 1.0f;
	UPROPERTY(EditAnywhere) float NormalOpacity = 1.0f;
};
#pragma once

#include "MeshComponent.h"
#include "StaticMeshComponent.generated.h"

UCLASS()
class UStaticMeshComponent : public UMeshComponent
{
	GENERATED_BODY()

public:
	UStaticMeshComponent();
	virtual ~UStaticMeshComponent();

	virtual void BeginPlay();
	virtual void TickComponent(float DeltaTime);

	virtual const FStaticMeshData* GetMeshData() const override { return StaticMesh ? &StaticMesh->GetMeshData() : nullptr; }

	// 메시가 바뀌면 슬롯 구성이 달라지므로 덮어쓰기를 비운다
	void SetStaticMesh(UStaticMesh* InStaticMesh);
	UStaticMesh* GetStaticMesh() const { return StaticMesh; }
	virtual UStaticMesh* GetRenderMesh() const override { return StaticMesh; }

	// 에디터 아이콘(라이트 화살표 등)은 게임 충돌에 참여하지 않는다
	virtual UBodySetup* GetBodySetup() override;

	virtual int32 GetNumMaterials() const override;
	virtual FString GetMaterialSlotName(int32 SlotIndex) const override;
	virtual UMaterialInterface* GetDefaultMaterial(int32 SlotIndex) const override;

	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;

	bool IsEditorIcon() const { return bEditorIcon; }

	void SetEditorIcon(bool IsEditorIcon) { bEditorIcon = IsEditorIcon; }
	UPROPERTY(EditAnywhere)
	bool bUseComplexAsSimpleCollision = false;

protected:
	virtual void OnCreatePhysicsState() override;

private:
	UPROPERTY(EditAnywhere)
	UStaticMesh* StaticMesh = nullptr;

	// 조명 Icon Debug arrow용
	bool bEditorIcon = false;

	UStaticMesh* PhysicsBodyMesh = nullptr;
};

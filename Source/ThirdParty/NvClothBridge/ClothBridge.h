#pragma once

#include <cstdint>

#ifdef NVCLOTH_BRIDGE_EXPORTS
#define CLOTH_BRIDGE_API __declspec(dllexport)
#else
#define CLOTH_BRIDGE_API __declspec(dllimport)
#endif

struct FClothBridgeVec3
{
	float X = 0.0f;
	float Y = 0.0f;
	float Z = 0.0f;
};

struct FClothBridgeVec4
{
	float X = 0.0f;
	float Y = 0.0f;
	float Z = 0.0f;
	float W = 0.0f;
};

struct FClothBridgeQuat
{
	float X = 0.0f;
	float Y = 0.0f;
	float Z = 0.0f;
	float W = 1.0f;
};

struct FClothBridgeMeshDesc
{
	const float* Positions = nullptr;
	uint32_t PositionStride = 0;
	uint32_t VertexCount = 0;
	const float* InvMasses = nullptr;
	uint32_t InvMassStride = 0;
	const uint32_t* Indices = nullptr;
	uint32_t TriangleCount = 0;
	FClothBridgeVec3 CookGravity = { 0.0f, 0.0f, -1.0f };
	bool bUseGeodesicTether = true;
};

struct FClothBridgeParams
{
	FClothBridgeVec3 Gravity = { 0.0f, 0.0f, -9.81f };
	FClothBridgeVec3 Damping = { 0.05f, 0.05f, 0.05f };
	FClothBridgeVec3 LinearDrag = { 0.1f, 0.1f, 0.1f };
	FClothBridgeVec3 AngularDrag = { 0.1f, 0.1f, 0.1f };
	FClothBridgeVec3 LinearInertia = { 1.0f, 1.0f, 1.0f };
	FClothBridgeVec3 AngularInertia = { 1.0f, 1.0f, 1.0f };
	FClothBridgeVec3 CentrifugalInertia = { 1.0f, 1.0f, 1.0f };
	FClothBridgeVec3 WindVelocity = { 0.0f, 0.0f, 0.0f };
	float DragCoefficient = 0.2f;
	float LiftCoefficient = 0.05f;
	float FluidDensity = 1.0f;
	float SolverFrequency = 120.0f;
	float StiffnessFrequency = 100.0f;
	float StretchStiffness = 1.0f;
	float BendStiffness = 0.5f;
	float ShearStiffness = 0.8f;
	float CompressionLimit = 1.0f;
	float StretchLimit = 1.0f;
	float StiffnessMultiplier = 1.0f;
	float MotionConstraintStiffness = 1.0f;
	float SelfCollisionDistance = 0.0f;
	float SelfCollisionStiffness = 0.5f;
	float TetherScale = 1.0f;
	float TetherStiffness = 1.0f;
	float Friction = 0.2f;
	bool bEnableContinuousCollision = false;
};

using FClothLogFn = void(*)(int32_t Level, const char* Message);
using FClothFactoryHandle = void*;
using FClothFabricHandle = void*;
using FClothSolverHandle = void*;
using FClothInstanceHandle = void*;

extern "C"
{
	// 전역 브릿지 초기화와 종료
	CLOTH_BRIDGE_API bool ClothBridge_Initialize(FClothLogFn LogCallback);
	CLOTH_BRIDGE_API void ClothBridge_Shutdown();

	// 팩토리 생성과 해제
	CLOTH_BRIDGE_API FClothFactoryHandle ClothBridge_CreateFactoryCPU();
	CLOTH_BRIDGE_API void ClothBridge_DestroyFactory(FClothFactoryHandle Factory);

	// 메쉬로 패브릭 쿠킹과 해제
	CLOTH_BRIDGE_API FClothFabricHandle ClothBridge_CookFabric(FClothFactoryHandle Factory, const FClothBridgeMeshDesc* MeshDesc);
	CLOTH_BRIDGE_API void ClothBridge_DestroyFabric(FClothFabricHandle Fabric);

	// 솔버 생성과 스텝 제어
	CLOTH_BRIDGE_API FClothSolverHandle ClothBridge_CreateSolver(FClothFactoryHandle Factory);
	CLOTH_BRIDGE_API void ClothBridge_DestroySolver(FClothSolverHandle Solver);
	CLOTH_BRIDGE_API void ClothBridge_AddClothToSolver(FClothSolverHandle Solver, FClothInstanceHandle Cloth);
	CLOTH_BRIDGE_API void ClothBridge_RemoveClothFromSolver(FClothSolverHandle Solver, FClothInstanceHandle Cloth);
	CLOTH_BRIDGE_API void ClothBridge_StepSolver(FClothSolverHandle Solver, float DeltaTime);

	// 천 인스턴스 생성과 상태 갱신
	CLOTH_BRIDGE_API FClothInstanceHandle ClothBridge_CreateCloth(FClothFactoryHandle Factory, FClothFabricHandle Fabric, const FClothBridgeVec4* InitialParticles, uint32_t ParticleCount);
	CLOTH_BRIDGE_API void ClothBridge_DestroyCloth(FClothInstanceHandle Cloth);
	CLOTH_BRIDGE_API void ClothBridge_ApplyParams(FClothInstanceHandle Cloth, FClothFabricHandle Fabric, const FClothBridgeParams* Params);
	CLOTH_BRIDGE_API void ClothBridge_SetTransform(FClothInstanceHandle Cloth, const FClothBridgeVec3* Translation, const FClothBridgeQuat* Rotation, bool bTeleport);
	CLOTH_BRIDGE_API void ClothBridge_SetSpheresAndCapsules(FClothInstanceHandle Cloth, const FClothBridgeVec4* Spheres, uint32_t SphereCount, const uint32_t* CapsuleIndices, uint32_t CapsuleIndexCount);
	CLOTH_BRIDGE_API void ClothBridge_SetVirtualParticles(FClothInstanceHandle Cloth, const uint32_t* TriangleIndices, uint32_t TriangleCount);
	CLOTH_BRIDGE_API void ClothBridge_SetMotionConstraints(FClothInstanceHandle Cloth, const FClothBridgeVec4* Constraints, uint32_t Count);
	CLOTH_BRIDGE_API void ClothBridge_SetSeparationConstraints(FClothInstanceHandle Cloth, const FClothBridgeVec4* Constraints, uint32_t Count);
	CLOTH_BRIDGE_API void ClothBridge_ClearInterpolation(FClothInstanceHandle Cloth);
	CLOTH_BRIDGE_API void ClothBridge_WriteParticles(FClothInstanceHandle Cloth, const FClothBridgeVec4* Particles, uint32_t Count);
	CLOTH_BRIDGE_API void ClothBridge_WriteParticlesWithPrevious(FClothInstanceHandle Cloth, const FClothBridgeVec4* CurrentParticles, const FClothBridgeVec4* PreviousParticles, uint32_t Count);
	CLOTH_BRIDGE_API void ClothBridge_UpdatePinnedParticles(FClothInstanceHandle Cloth, const uint32_t* Indices, const FClothBridgeVec3* LocalPositions, uint32_t Count);
	CLOTH_BRIDGE_API void ClothBridge_ReadParticles(FClothInstanceHandle Cloth, FClothBridgeVec4* OutParticles, uint32_t Count);
	CLOTH_BRIDGE_API void ClothBridge_ReadPreviousParticles(FClothInstanceHandle Cloth, FClothBridgeVec4* OutParticles, uint32_t Count);
}

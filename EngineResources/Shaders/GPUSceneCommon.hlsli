#ifndef GPU_SCENE_COMMON_HLSLI
#define GPU_SCENE_COMMON_HLSLI

struct FPrimitiveSceneData
{
	matrix LocalToWorld;
	matrix NormalMatrix;
	float4 Flags;
};

StructuredBuffer<FPrimitiveSceneData> PrimitiveSceneData : register(t17);
StructuredBuffer<uint> InstancePrimitiveIds : register(t18);

cbuffer DrawBatchConstants : register(b5)
{
	uint StartInstance;
	uint HeatmapWireMode;
	uint2 DrawBatchPad;
};

uint LoadScenePrimitiveId(uint InstanceId)
{
#if VF_MESH_PARTICLE
	return 0u;
#else
	return InstancePrimitiveIds[StartInstance + InstanceId];
#endif
}

FPrimitiveSceneData LoadPrimitiveSceneData(uint InstanceId)
{
	const uint PrimitiveId = InstancePrimitiveIds[StartInstance + InstanceId];
	return PrimitiveSceneData[PrimitiveId];
}

FPrimitiveSceneData LoadScenePrimitive(uint InstanceId)
{
#if VF_MESH_PARTICLE
	FPrimitiveSceneData Result;
	Result.LocalToWorld = float4x4(
		1.0f, 0.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		0.0f, 0.0f, 0.0f, 1.0f);
	Result.NormalMatrix = Result.LocalToWorld;
	Result.Flags = float4(1.0f, 0.0f, 0.0f, 0.0f);
	return Result;
#else
	return LoadPrimitiveSceneData(InstanceId);
#endif
}

#endif

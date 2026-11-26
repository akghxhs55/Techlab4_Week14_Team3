#pragma pack_matrix(row_major)
#include "VertexFactory.hlsli"
#include "GPUSceneCommon.hlsli"

// 기존 메시 경로의 시점 상수 슬롯을 공유한다.
cbuffer ViewConstants : register(b0)
{
    matrix VP;
    matrix View;
    matrix Projection;
    float4 ViewTimeParams;
};

// ForwardOpaque(UberLit)와 동일한 View -> Projection 순서로 클립 좌표를 계산해 깊이를 일치시킨다.
float4 mainVS(FVertexFactoryInput Input, uint InstanceId : SV_InstanceID) : SV_POSITION
{
    const FPrimitiveSceneData Prim = LoadScenePrimitive(InstanceId);
    const FVFIntermediates VF = VF_GetIntermediates(Input, Prim.LocalToWorld, Prim.LocalToWorld);
    float4 WorldPosition = float4(VF.WorldPosition, 1.0f);
    float4 ViewPosition = mul(WorldPosition, View);
    return mul(ViewPosition, Projection);
}

void mainPS()
{
    return;
}

// Landscape: 정점에는 격자 좌표만 있고, 높이와 노멀은 하이트맵 텍스처에서 읽는다 (UE FLandscapeVertexFactory)
// 입력 레이아웃은 VertexFactory.cpp의 GLandscapeElements와 같아야 한다
struct FVertexFactoryInput
{
    uint4 Packed : POSITION; // x, y: 이 LOD 격자 좌표, z: LOD 번호, w: 미사용
};

Texture2D<float4> LandscapeHeightmap : register(t19);

// FLandscapeShaderParams(LandscapeRender.h, 2단계)와 레이아웃이 같아야 한다
cbuffer LandscapeParams : register(b6)
{
    float4 LandscapeSectionParams; // x: 섹션 쿼드 수(63), y: LOD0 한 변 정점 수(64), z: 섹션 LOD(소수, 이웃 차이 ≤1로 보정됨), w: LOD 디버그 색 on/off
    int4 LandscapeHeightmapOffset; // xy: 하이트맵 안에서 이 섹션의 시작 텍셀 (청크별 텍스처면 0)
    float4 LandscapeNeighborLODs; // x: -X, y: +X, z: -Y, w: +Y 이웃 섹션 LOD (이웃이 없으면 자기 LOD)
};

float3 LandscapeLODColor(float LOD)
{
    const float3 Colors[4] = { float3(1.0f, 0.2f, 0.2f), float3(0.2f, 1.0f, 0.2f), float3(0.2f, 0.4f, 1.0f), float3(1.0f, 1.0f, 0.2f) };
    const int L = min((int) LOD, 2);
    return lerp(Colors[L], Colors[L + 1], saturate(LOD - (float) L));
}

// LandscapeDataAccess::GetLocalHeight와 같은 식이어야 CPU 질의·PhysX와 높이가 일치한다
float LandscapeDecodeHeight(float4 Texel)
{
    const uint High = (uint) round(Texel.r * 255.0f);
    const uint Low = (uint) round(Texel.g * 255.0f);
    const float Raw = (float) ((High << 8) | Low);
    return (Raw - 32768.0f) * (1.0f / 128.0f);
}

float3 LandscapeDecodeNormal(float4 Texel)
{
    const float2 XY = Texel.ba * 2.0f - 1.0f;
    return float3(XY, sqrt(saturate(1.0f - dot(XY, XY))));
}

// 메시 LOD(MeshLOD)로 그린 정점 Grid가 더 거친 LOD L에서 겹쳐지는 정점의 위치와 텍셀 (L >= MeshLOD)
// Grid >> 1 = 다음 LOD 정점이라, 모핑이 끝나면(alpha=1) LOD k 메시가 LOD k+1 메시와 정확히 같아진다
void LandscapeSampleAtLOD(uint2 Grid, uint MeshLOD, uint L, out float2 LocalXY, out float4 Texel)
{
    const uint2 G = Grid >> (L - MeshLOD);
    const uint LastG = (((uint) LandscapeSectionParams.y) >> L) - 1;
    LocalXY = float2(G) * (LandscapeSectionParams.x / (float) LastG);
    LocalXY = (G == LastG) ? LandscapeSectionParams.xx : LocalXY; // 마지막 정점은 정확히 섹션 크기 (이웃 섹션의 0과 맞물리게)
    // Load: 필터링 없이 정확한 값을 읽어야 CPU·PhysX 높이와 비트 단위로 같다
    Texel = LandscapeHeightmap.Load(int3(int2(G) + (LandscapeHeightmapOffset.xy >> L), L));
}

// 정점의 LOD: 섹션 중앙은 섹션 LOD, 가장자리는 max(내 LOD, 이웃 LOD). UV01은 모핑 전 메시 격자 기준 0~1
// 가장 가까운 변 하나만 보므로 변 위의 정점은 그 변을 공유하는 두 섹션이 같은 값을 쓴다 (Crack 방지)
float LandscapeVertexLOD(float2 UV01)
{
    const float SectionLOD = LandscapeSectionParams.z;
    const float2 FromCenter = UV01 - 0.5f;
    const float2 Dist = abs(FromCenter);
    
    float NeighborLOD;
    if(Dist.x >= Dist.y)
        NeighborLOD = (FromCenter.x < 0.0f) ? LandscapeNeighborLODs.x : LandscapeNeighborLODs.y;
    else
        NeighborLOD = (FromCenter.y < 0.0f) ? LandscapeNeighborLODs.z : LandscapeNeighborLODs.w;
    const float EdgeLOD = max(SectionLOD, NeighborLOD);
    const float T = saturate(max(Dist.x, Dist.y) * 2.0f);
    
    return (T >= 1.0f) ? EdgeLOD : lerp(SectionLOD, EdgeLOD, T);
}

FVFIntermediates VF_GetIntermediates(FVertexFactoryInput Input, float4x4 World, float4x4 NormalMatrix)
{
    const uint MeshLOD = Input.Packed.z;
    const uint2 Grid = Input.Packed.xy;

    // 이 정점이 보여야 할 LOD (소수): 섹션 중앙 = 섹션 LOD, 변 = max(나, 이웃)
    const float MeshQuads = (float) ((((uint) LandscapeSectionParams.y) >> MeshLOD) - 1);
    const float TargetLOD = max(LandscapeVertexLOD(float2(Grid) / MeshQuads), (float) MeshLOD);

    const uint LodA = (uint) TargetLOD;
    const float MorphAlpha = TargetLOD - (float) LodA;

    float2 XYA, XYB;
    float4 TexelA, TexelB;
    LandscapeSampleAtLOD(Grid, MeshLOD, LodA, XYA, TexelA);
    LandscapeSampleAtLOD(Grid, MeshLOD, LodA + 1, XYB, TexelB);

    const float2 LocalXY = lerp(XYA, XYB, MorphAlpha);
    const float Height = lerp(LandscapeDecodeHeight(TexelA), LandscapeDecodeHeight(TexelB), MorphAlpha);
    const float3 LocalNormal = normalize(lerp(LandscapeDecodeNormal(TexelA), LandscapeDecodeNormal(TexelB), MorphAlpha));

    const float3 LocalPos = float3(LocalXY, Height);
    // Z-up: 접선은 +X 방향을 표면에 투영
    const float3 LocalTangent = normalize(float3(1.0f, 0.0f, 0.0f) - LocalNormal * LocalNormal.x);

    // 모핑된 위치 기준 0~1 → 정점이 움직여도 텍스처가 미끄러지지 않는다 (Phase 3에서 Splat/타일링 UV로 교체)
    const float2 UV = LocalXY / LandscapeSectionParams.x;

    float4 Color = float4(1.0f, 1.0f, 1.0f, 1.0f);
    if (LandscapeSectionParams.w > 0.5f)
    {
        Color = float4(LandscapeLODColor(TargetLOD), 1.0f);
    }

    return VF_LocalToWorld(LocalPos, LocalNormal, float4(LocalTangent, 1.0f), UV, Color, World, NormalMatrix);
}
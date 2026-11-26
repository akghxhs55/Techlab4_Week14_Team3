#pragma pack_matrix(row_major)

cbuffer constants : register(b0)
{
    matrix VP;
    matrix View;
    matrix Projection;
    float4 ViewTimeParams;
};

cbuffer SpriteConstants : register(b1)
{
    float4 CameraRight;
    float4 CameraUp;
    float4 SubImageSize; // (가로 칸, 세로 칸, 1/가로, 1/세로)
};

Texture2D SpriteTexture : register(t0);
SamplerState SpriteSampler : register(s0);
SamplerState ColorSampler : register(s0);

#ifdef MATERIAL_GRAPH
#include MATERIAL_GRAPH_INCLUDE
#endif

struct VS_INPUT
{
    float2 uv : POSITION; // 슬롯 0: 모서리
    float3 position : INSTANCE_POSITION; // 슬롯 1: 파티클
    float rotation : INSTANCE_ROTATION;
    float2 size : INSTANCE_SIZE;
    float subImageIndex : INSTANCE_SUBIMAGE;
    float4 color : INSTANCE_COLOR;
};

struct PS_INPUT
{
    float4 position : SV_POSITION;
    float2 uvA : TEXCOORD0;
    float2 uvB : TEXCOORD1;
    float blend : TEXCOORD2;
    float4 color : COLOR0;
    float3 worldPosition : TEXCOORD3;
};

float2 CellUV(float index, float2 cornerUV)
{
    float2 cell = float2(fmod(index, SubImageSize.x), floor(index / SubImageSize.x));
    return (cell + cornerUV) * SubImageSize.zw;
}

PS_INPUT mainVS(VS_INPUT input)
{
    // 모서리 UV(0~1)를 중심 기준 오프셋(-0.5~0.5)으로. V는 아래로 증가하므로 뒤집는다
    float2 offset = float2(input.uv.x - 0.5f, 0.5f - input.uv.y) * input.size;

    float s, c;
    sincos(input.rotation, s, c);
    float3 right = CameraRight.xyz * c + CameraUp.xyz * s;
    float3 up = CameraUp.xyz * c - CameraRight.xyz * s;
    float3 world = input.position + right * offset.x + up * offset.y;

    float total = SubImageSize.x * SubImageSize.y;
    float indexA = floor(input.subImageIndex);
    float indexB = min(indexA + 1.0f, total - 1.0f);

    PS_INPUT output;
    output.position = mul(float4(world, 1.0f), VP);
    output.uvA = CellUV(indexA, input.uv);
    output.uvB = CellUV(indexB, input.uv);
    output.blend = frac(input.subImageIndex);
    output.color = input.color;
    output.worldPosition = world;
    return output;
}

float4 mainPS(PS_INPUT input) : SV_TARGET
{
#ifdef MATERIAL_GRAPH
    FMaterialGraphOutput GraphOutput = EvaluateMaterialGraph(input.uvA, input.worldPosition, float3(0, 0, 1), float3(0, 0, 1), input.color, ViewTimeParams.x);
#ifdef MATERIAL_GRAPH_HAS_OPACITY_MASK
    clip(GraphOutput.OpacityMask - 0.001f);
#endif
    float4 texel;
#ifdef MATERIAL_GRAPH_HAS_BASE_COLOR
    texel = GraphOutput.BaseColor;
#else
    texel = SpriteTexture.Sample(SpriteSampler, input.uvA);
    if (input.blend > 0.0f)
    {
        texel = lerp(texel, SpriteTexture.Sample(SpriteSampler, input.uvB), input.blend);
    }
#endif
#ifdef MATERIAL_GRAPH_HAS_EMISSIVE_COLOR
    float3 EmissiveOut = GraphOutput.EmissiveColor;
#ifdef MATERIAL_GRAPH_HAS_EMISSIVE_FACTOR
    EmissiveOut *= GraphOutput.EmissiveFactor;
#endif
    texel.rgb += EmissiveOut;
#endif
    return texel * input.color;
#else
    float4 texel = SpriteTexture.Sample(SpriteSampler, input.uvA);
    if (input.blend > 0.0f)
    {
        texel = lerp(texel, SpriteTexture.Sample(SpriteSampler, input.uvB), input.blend);
    }
    return texel * input.color;
#endif
}

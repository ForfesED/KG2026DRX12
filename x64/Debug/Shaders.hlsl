// =====================================================================
// Shaders.hlsl
// Deferred rendering: geometry pass заполняет G-buffer (Gbuffer struct),
// lighting pass читает G-buffer и накапливает освещение от каждого
// источника света через full-screen triangle (слайды 17-22).
// =====================================================================

// ---------------- Geometry pass: константы ----------------
cbuffer cbPerObject : register(b0)
{
    float4x4 gWorld;
    float4x4 gTexTransform;
    float4 gDiffuseAlbedo;
};

cbuffer cbPass : register(b1)
{
    float4x4 gViewProj;
};

Texture2D gDiffuseMap : register(t0);
SamplerState gSampler : register(s0);

struct VertexIn
{
    float3 PosL : POSITION;
    float3 NormalL : NORMAL;
    float2 TexC : TEXCOORD;
};

struct GeometryVSOut
{
    float4 PosH : SV_POSITION;
    float3 PosW : POSITION1;
    float3 NormalW : NORMAL;
    float2 TexC : TEXCOORD;
};

// ---------------- Geometry pass: вершинный шейдер ----------------
GeometryVSOut GeometryVS(VertexIn vin)
{
    GeometryVSOut vout;

    float4 posW = mul(float4(vin.PosL, 1.0f), gWorld);
    vout.PosW = posW.xyz;
    vout.PosH = mul(posW, gViewProj);
    vout.NormalW = normalize(mul(vin.NormalL, (float3x3) gWorld));

    float4 texC = mul(float4(vin.TexC, 0.0f, 1.0f), gTexTransform);
    vout.TexC = texC.xy;

    return vout;
}

// ---------------- Geometry pass: выходные данные в G-buffer (слайд 18) ----------------
struct Gbuffer
{
    float4 DiffuseSpec : SV_Target0; // Albedo (rgb) + Spec power в альфе
    float4 WorldPos : SV_Target1; // мировые координаты
    float4 Normal : SV_Target2; // нормаль в мировом пространстве
};

[earlydepthstencil]
Gbuffer GeometryPS(GeometryVSOut pin)
{
    Gbuffer gout;

    float4 texColor = gDiffuseMap.Sample(gSampler, pin.TexC);
    float4 albedo = texColor * gDiffuseAlbedo;

    gout.DiffuseSpec = float4(albedo.rgb, 0.5f); // 0.5 - условный specular power, можно брать из материала
    gout.WorldPos = float4(pin.PosW, 1.0f);
    gout.Normal = float4(normalize(pin.NormalW), 0.0f);

    return gout;
}

// =====================================================================
// LIGHTING PASS
// =====================================================================

// Типы источников света - должны совпадать с enum LightType в RenderingSystem.h
#define LIGHT_DIRECTIONAL 0
#define LIGHT_POINT       1
#define LIGHT_SPOT        2

struct Light
{
    float3 Position;
    float Range;

    float3 Direction;
    float SpotPower;

    float3 Color;
    float Intensity;

    int Type;
    float3 _pad;
};

cbuffer cbLight : register(b0)
{
    float4x4 gLightViewProj;
    float4x4 gLightInvViewProj;
    float3 gCameraPos;
    float gLightPad0;
    Light gLight;
};

// G-Buffer как источник данных для lighting pass (слайд 20)
Texture2D<float4> gAlbedoTex : register(t0);
Texture2D<float4> gWorldPosTex : register(t1);
Texture2D<float4> gNormalTex : register(t2);

struct LightingVSOut
{
    float4 PosH : SV_POSITION;
    float2 TexC : TEXCOORD;
};

// Full-screen triangle (вариант screen-aligned quad из слайда 22, на 3 вершинах)
LightingVSOut LightingVS(uint id : SV_VertexID)
{
    LightingVSOut vout;
    // (0,0)->(-1,-1) ... простая раскладка треугольника, перекрывающего весь экран
    float2 tex = float2((id << 1) & 2, id & 2);
    vout.TexC = tex;
    vout.PosH = float4(tex * float2(2, -2) + float2(-1, 1), 0, 1);
    return vout;
}

// ---------------- Lighting pass: пиксельный шейдер ----------------
// Читаем G-buffer через Load по экранным координатам (слайд 20)/
float4 LightingPS(LightingVSOut pin) : SV_Target
{
    int3 sampleCoord = int3((int2) pin.PosH.xy, 0);

    float4 albedoSpec = gAlbedoTex.Load(sampleCoord);
    float3 worldPos = gWorldPosTex.Load(sampleCoord).xyz;
    float3 normal = normalize(gNormalTex.Load(sampleCoord).xyz);

    float3 albedo = albedoSpec.rgb;
    float specPower = albedoSpec.a;

    float3 toEye = normalize(gCameraPos - worldPos);

    float3 lightColor = gLight.Color * gLight.Intensity;
    float3 lighting = float3(0, 0, 0);

    if (gLight.Type == LIGHT_DIRECTIONAL)
    {
        // Directional - может добавлять ambient часть (слайд 13)
        float3 lightDir = normalize(-gLight.Direction);
        float ndotl = saturate(dot(normal, lightDir));

        float3 ambient = albedo * 0.15f * lightColor;
        float3 diffuse = albedo * ndotl * lightColor;

        // простой specular (Blinn-Phong)
        float3 halfVec = normalize(lightDir + toEye);
        float spec = pow(saturate(dot(normal, halfVec)), 16.0f) * specPower;

        lighting = ambient + diffuse + spec * lightColor;
    }
    else if (gLight.Type == LIGHT_POINT)
    {
        float3 lightVec = gLight.Position - worldPos;
        float dist = length(lightVec);

        if (dist > gLight.Range)
            return float4(0, 0, 0, 0); // пиксель вне дальности - не вносит вклад

        float3 lightDir = lightVec / max(dist, 0.0001f);
        float ndotl = saturate(dot(normal, lightDir));

        // затухание на расстоянии (слайд 5)
        float attenuation = saturate(1.0f - (dist * dist) / (gLight.Range * gLight.Range));

        float3 diffuse = albedo * ndotl * lightColor * attenuation;

        float3 halfVec = normalize(lightDir + toEye);
        float spec = pow(saturate(dot(normal, halfVec)), 16.0f) * specPower;

        lighting = diffuse + spec * lightColor * attenuation;
    }
    else // LIGHT_SPOT
    {
        float3 lightVec = gLight.Position - worldPos;
        float dist = length(lightVec);

        if (dist > gLight.Range)
            return float4(0, 0, 0, 0);

        float3 lightDir = lightVec / max(dist, 0.0001f);
        float ndotl = saturate(dot(normal, lightDir));

        float attenuation = saturate(1.0f - (dist * dist) / (gLight.Range * gLight.Range));

        // конус прожектора - чем дальше от направления, тем темнее
        float3 spotDir = normalize(gLight.Direction);
        float spotFactor = pow(saturate(dot(-lightDir, spotDir)), gLight.SpotPower);

        float3 diffuse = albedo * ndotl * lightColor * attenuation * spotFactor;

        float3 halfVec = normalize(lightDir + toEye);
        float spec = pow(saturate(dot(normal, halfVec)), 16.0f) * specPower;

        lighting = diffuse + (spec * lightColor * attenuation * spotFactor);
    }

    return float4(lighting, 1.0f);
}
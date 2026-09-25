// Shaders.hlsl
// Deferred rendering с аппаратной тесселяцией.
//
// Geometry pass идёт по цепочке VS -> HS -> Tessellator -> DS -> PS:
//  VS  переводит вершины в мировое пространство и ничего не проецирует
//  HS  копирует контрольные точки и считает коэффициенты тесселяции по расстоянию до камеры
//  Tessellator (фиксированный блок GPU) по этим коэффициентам нарезает треугольник на мелкие
//  DS  для каждой новой вершины интерполирует атрибуты, читает displacement-текстуру
//      и сдвигает вершину вдоль нормали, потом проецирует её на экран
//  PS  берёт нормаль из карты нормалей, переводит её в мир через матрицу TBN и пишет всё в G-buffer
//
// Lighting pass читает G-buffer и накапливает свет от каждого источника full-screen треугольником.


// Карты нормалей бывают в двух конвенциях: DirectX (зелёный канал "вниз") и OpenGL (зелёный "вверх").
// Если выпуклости освещаются так, будто они вдавлены, поставь тут 1 - зелёный канал перевернётся.
#define NORMAL_MAP_FLIP_Y 0


// Константы объекта: мировая матрица, трансформы UV и цвет материала.
// UV для цвета и для рельефа разные: цветная текстура едет (тайлинг + анимация),
// а карты нормалей и высот только тайлятся и стоят на месте, иначе геометрия "плывёт"
cbuffer cbPerObject : register(b0)
{
    float4x4 gWorld;
    float4x4 gTexTransform; // тайлинг и анимация, для diffuse
    float4 gDiffuseAlbedo;
    float4x4 gReliefTexTransform; // только тайлинг, для normal map и displacement map
};

// Константы кадра: камера и параметры тесселяции и смещения
cbuffer cbPass : register(b1)
{
    float4x4 gViewProj;
    float3 gEyePosW; // позиция камеры в мире, от неё меряем расстояние до рёбер патча
    float gTessMinDist; // ближе этого расстояния коэффициент тесселяции максимальный
    float gTessMaxDist; // дальше этого расстояния коэффициент тесселяции минимальный
    float gTessMinFactor; // минимальный коэффициент (1 = треугольник не делится)
    float gTessMaxFactor; // максимальный коэффициент (сколько сегментов на ребре вблизи)
    float gDisplacementScale; // высота смещения в мировых единицах для белого пикселя карты
    float gDisplacementBias; // постоянная добавка к смещению, можно "утопить" поверхность
    float3 gPassPad; // выравнивание cbuffer до 16 байт
};

// Текстуры материала идут тремя дескрипторами подряд в одной таблице
Texture2D gDiffuseMap : register(t0); // цвет
Texture2D gNormalMap : register(t1); // нормали в tangent space, упакованные в [0,1]
Texture2D gDisplacementMap : register(t2); // карта высот, берём красный канал
SamplerState gSampler : register(s0);

// Вершина из вершинного буфера, раскладка совпадает со struct Vertex в main.cpp
struct VertexIn
{
    float3 PosL : POSITION;
    float3 NormalL : NORMAL;
    float2 TexC : TEXCOORD;
    float3 TangentL : TANGENT;
    float3 BinormalL : BINORMAL;
    float3 DispNormalL : DISPNORMAL; // общая нормаль сдвига для всех копий вершины в этой точке
    float DispWeight : DISPWEIGHT; // 0 на шве UV, 1 в остальных местах
};

// Контрольная точка патча: это и выход VS, и вход и выход HS.
// SV_POSITION здесь нет, потому что проецировать на экран можно только после смещения в DS.
struct ControlPoint
{
    float3 PosW : POSITION;
    float3 NormalW : NORMAL;
    float3 TangentW : TANGENT;
    float3 BinormalW : BINORMAL;
    float2 TexC : TEXCOORD0; // UV для цвета, едут со временем
    float2 TexCRelief : TEXCOORD1; // UV для рельефа, стоят на месте
    float3 DispNormalW : DISPNORMAL;
    float DispWeight : DISPWEIGHT;
};

// Коэффициенты тесселяции для треугольного патча: по одному на каждое ребро и один на внутреннюю часть
struct PatchTess
{
    float EdgeTess[3] : SV_TessFactor;
    float InsideTess : SV_InsideTessFactor;
};

// Выход DS, он же вход PS
struct DomainOut
{
    float4 PosH : SV_POSITION;
    float3 PosW : POSITION1;
    float3 NormalW : NORMAL;
    float3 TangentW : TANGENT;
    float3 BinormalW : BINORMAL;
    float2 TexC : TEXCOORD0;
    float2 TexCRelief : TEXCOORD1;
};


// Вершинный шейдер.
// Только переводит всё в мировое пространство, т.к. и коэффициенты тесселяции,
// и смещение по displacement удобнее считать в мире.
ControlPoint GeometryVS(VertexIn vin)
{
    ControlPoint vout;

    vout.PosW = mul(float4(vin.PosL, 1.0f), gWorld).xyz;

    // для векторов берём только поворот и масштаб из мировой матрицы, без переноса
    float3x3 world3x3 = (float3x3) gWorld;
    vout.NormalW = normalize(mul(vin.NormalL, world3x3));
    vout.TangentW = mul(vin.TangentL, world3x3);
    vout.BinormalW = mul(vin.BinormalL, world3x3);
    vout.DispNormalW = normalize(mul(vin.DispNormalL, world3x3));
    vout.DispWeight = vin.DispWeight;
    // UV цвета: тайлинг и анимация
    vout.TexC = mul(float4(vin.TexC, 0.0f, 1.0f), gTexTransform).xy;
    // UV рельефа: только тайлинг, без сдвига по времени
    vout.TexCRelief = mul(float4(vin.TexC, 0.0f, 1.0f), gReliefTexTransform).xy;

    return vout;
}


// Коэффициент тесселяции по расстоянию до камеры.
// t = 0 вблизи (d <= gTessMinDist) и t = 1 вдали (d >= gTessMaxDist),
// дальше обычный lerp между максимальным и минимальным коэффициентом.
float CalcTessFactor(float3 posW)
{
    float d = distance(posW, gEyePosW);
    float t = saturate((d - gTessMinDist) / (gTessMaxDist - gTessMinDist));
    return lerp(gTessMaxFactor, gTessMinFactor, t);
}

// Константная функция патча: вызывается один раз на треугольник.
// Коэффициент ребра считаем по его середине. Соседние треугольники делят одно ребро,
// у него одна и та же середина, значит и коэффициент одинаковый - трещин между патчами не будет.
// В домене "tri" ребро i лежит напротив вершины i.
PatchTess ConstantHS(InputPatch<ControlPoint, 3> patch, uint patchId : SV_PrimitiveID)
{
    PatchTess pt;

    float3 edge0Mid = 0.5f * (patch[1].PosW + patch[2].PosW);
    float3 edge1Mid = 0.5f * (patch[2].PosW + patch[0].PosW);
    float3 edge2Mid = 0.5f * (patch[0].PosW + patch[1].PosW);
    float3 center = (patch[0].PosW + patch[1].PosW + patch[2].PosW) / 3.0f;

    pt.EdgeTess[0] = CalcTessFactor(edge0Mid);
    pt.EdgeTess[1] = CalcTessFactor(edge1Mid);
    pt.EdgeTess[2] = CalcTessFactor(edge2Mid);
    pt.InsideTess = CalcTessFactor(center);

    return pt;
}

// Hull shader, фаза контрольных точек: вызывается один раз на каждую из 3 вершин патча.
// Просто пропускает данные дальше, такой pass-through шейдер драйвер оптимизирует сам.
// fractional_odd даёт плавное изменение сетки при изменении коэффициента, без резких скачков.
[domain("tri")]
[partitioning("fractional_odd")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("ConstantHS")]
[maxtessfactor(64.0f)]
ControlPoint GeometryHS(InputPatch<ControlPoint, 3> patch,
                        uint cpId : SV_OutputControlPointID,
                        uint patchId : SV_PrimitiveID)
{
    return patch[cpId];
}


// Интерполяция по барицентрическим координатам внутри треугольника
float3 BaryLerp(float3 a, float3 b, float3 c, float3 bary)
{
    return a * bary.x + b * bary.y + c * bary.z;
}

float2 BaryLerp(float2 a, float2 b, float2 c, float3 bary)
{
    return a * bary.x + b * bary.y + c * bary.z;
}
float BaryLerp(float a, float b, float c, float3 bary)
{
    return a * bary.x + b * bary.y + c * bary.z;
}
// Domain shader: вызывается один раз на каждую вершину, которую создал тесселятор.
// bary - положение новой вершины внутри исходного треугольника.
[domain("tri")]
DomainOut GeometryDS(PatchTess patchTess,
                     float3 bary : SV_DomainLocation,
                     const OutputPatch<ControlPoint, 3> tri)
{
    DomainOut dout;

    float3 posW = BaryLerp(tri[0].PosW, tri[1].PosW, tri[2].PosW, bary);
    float3 normalW = normalize(BaryLerp(tri[0].NormalW, tri[1].NormalW, tri[2].NormalW, bary));
    float3 tangentW = BaryLerp(tri[0].TangentW, tri[1].TangentW, tri[2].TangentW, bary);
    float3 binormalW = BaryLerp(tri[0].BinormalW, tri[1].BinormalW, tri[2].BinormalW, bary);
    float2 texC = BaryLerp(tri[0].TexC, tri[1].TexC, tri[2].TexC, bary);
    float2 texCRelief = BaryLerp(tri[0].TexCRelief, tri[1].TexCRelief, tri[2].TexCRelief, bary);

    // В DS нет производных экранных координат, поэтому обычный Sample нельзя,
    // уровень мипа указываем явно через SampleLevel.
    // Высоту читаем по неподвижным UV, чтобы геометрия не двигалась вместе с цветной текстурой
    float height = gDisplacementMap.SampleLevel(gSampler, texCRelief, 0).r;

        // Сдвигаем вдоль общей нормали, а не вдоль нормали грани: на остром ребре копии вершины
    // получают одинаковое направление и не разъезжаются.
    // Вес плавно гасит сдвиг к швам UV, где у копий разные высоты из текстуры.
    float3 dispNormalW = normalize(BaryLerp(tri[0].DispNormalW, tri[1].DispNormalW, tri[2].DispNormalW, bary));
    float dispWeight = BaryLerp(tri[0].DispWeight, tri[1].DispWeight, tri[2].DispWeight, bary);

    // белый пиксель карты поднимает вершину на gDisplacementScale, чёрный оставляет на месте
    float displacement = (height * gDisplacementScale + gDisplacementBias) * dispWeight;
    posW += dispNormalW * displacement;

    dout.PosH = mul(float4(posW, 1.0f), gViewProj);
    dout.PosW = posW;
    dout.NormalW = normalW;
    dout.TangentW = tangentW;
    dout.BinormalW = binormalW;
    dout.TexC = texC;
    dout.TexCRelief = texCRelief;

    return dout;
}


// Что пиксельный шейдер пишет в G-buffer (3 рендертаргета сразу)
struct Gbuffer
{
    float4 DiffuseSpec : SV_Target0; // albedo в rgb и сила блика в альфе
    float4 WorldPos : SV_Target1; // мировые координаты
    float4 Normal : SV_Target2; // нормаль в мировом пространстве
};

// Пиксельный шейдер geometry pass с normal mapping
[earlydepthstencil]
Gbuffer GeometryPS(DomainOut pin)
{
    Gbuffer gout;

    float4 texColor = gDiffuseMap.Sample(gSampler, pin.TexC);
    float4 albedo = texColor * gDiffuseAlbedo;

    // После интерполяции векторы базиса уже не единичные и не перпендикулярны друг другу.
    // Процесс Грама-Шмидта: убираем из T составляющую вдоль N, а B получаем векторным произведением.
    float3 N = normalize(pin.NormalW);
    float3 T = normalize(pin.TangentW - dot(pin.TangentW, N) * N);
    float3 B = cross(N, T);

    // если UV отзеркалены, бинормаль должна смотреть в другую сторону, это видно по исходной бинормали
    if (dot(B, pin.BinormalW) < 0.0f)
        B = -B;

    // распаковка нормали из [0,1] в [-1,1], читаем по тем же неподвижным UV, что и высоту
    float3 normalT = gNormalMap.Sample(gSampler, pin.TexCRelief).rgb * 2.0f - 1.0f;
#if NORMAL_MAP_FLIP_Y
    normalT.y = -normalT.y;
#endif

    // строки матрицы TBN - это оси tangent space в мировом пространстве,
    // поэтому умножение вектора-строки на неё переводит нормаль из tangent space в мир
    float3x3 TBN = float3x3(T, B, N);
    float3 normalW = normalize(mul(normalT, TBN));

    gout.DiffuseSpec = float4(albedo.rgb, 0.5f); // 0.5 - условная сила блика, можно брать из материала
    gout.WorldPos = float4(pin.PosW, 1.0f);
    gout.Normal = float4(normalW, 0.0f);

    return gout;
}


// LIGHTING PASS

// Типы источников света, должны совпадать с enum LightType в RenderingSystem.h
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

// Константы одного источника света и камеры
cbuffer cbLight : register(b0)
{
    float4x4 gLightViewProj;
    float4x4 gLightInvViewProj;
    float3 gCameraPos;
    float gLightPad0;
    Light gLight;
};

// G-buffer как входные текстуры
Texture2D<float4> gAlbedoTex : register(t0);
Texture2D<float4> gWorldPosTex : register(t1);
Texture2D<float4> gNormalTex : register(t2);

struct LightingVSOut
{
    float4 PosH : SV_POSITION;
    float2 TexC : TEXCOORD;
};

// Full-screen треугольник из 3 вершин без вершинного буфера.
// По SV_VertexID получаются UV (0,0), (2,0), (0,2) - треугольник, который накрывает весь экран.
LightingVSOut LightingVS(uint id : SV_VertexID)
{
    LightingVSOut vout;
    float2 tex = float2((id << 1) & 2, id & 2);
    vout.TexC = tex;
    vout.PosH = float4(tex * float2(2, -2) + float2(-1, 1), 0, 1);
    return vout;
}

// Пиксельный шейдер освещения: читает G-buffer через Load по экранным координатам пикселя
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
        // направленный свет, только он добавляет ambient
        float3 lightDir = normalize(-gLight.Direction);
        float ndotl = saturate(dot(normal, lightDir));

        float3 ambient = albedo * 0.15f * lightColor;
        float3 diffuse = albedo * ndotl * lightColor;

        // блик по Блинну-Фонгу
        float3 halfVec = normalize(lightDir + toEye);
        float spec = pow(saturate(dot(normal, halfVec)), 16.0f) * specPower;

        lighting = ambient + diffuse + spec * lightColor;
    }
    else if (gLight.Type == LIGHT_POINT)
    {
        float3 lightVec = gLight.Position - worldPos;
        float dist = length(lightVec);

        // пиксель дальше радиуса действия света ничего не получает
        if (dist > gLight.Range)
            return float4(0, 0, 0, 0);

        float3 lightDir = lightVec / max(dist, 0.0001f);
        float ndotl = saturate(dot(normal, lightDir));

        // затухание с расстоянием
        float attenuation = saturate(1.0f - (dist * dist) / (gLight.Range * gLight.Range));

        float3 diffuse = albedo * ndotl * lightColor * attenuation;

        float3 halfVec = normalize(lightDir + toEye);
        float spec = pow(saturate(dot(normal, halfVec)), 16.0f) * specPower;

        lighting = diffuse + spec * lightColor * attenuation;
    }
    else
    {
        // прожектор: как точечный свет, но ещё умножается на конус
        float3 lightVec = gLight.Position - worldPos;
        float dist = length(lightVec);

        if (dist > gLight.Range)
            return float4(0, 0, 0, 0);

        float3 lightDir = lightVec / max(dist, 0.0001f);
        float ndotl = saturate(dot(normal, lightDir));

        float attenuation = saturate(1.0f - (dist * dist) / (gLight.Range * gLight.Range));

        // чем дальше пиксель от оси прожектора, тем он темнее
        float3 spotDir = normalize(gLight.Direction);
        float spotFactor = pow(saturate(dot(-lightDir, spotDir)), gLight.SpotPower);

        float3 diffuse = albedo * ndotl * lightColor * attenuation * spotFactor;

        float3 halfVec = normalize(lightDir + toEye);
        float spec = pow(saturate(dot(normal, halfVec)), 16.0f) * specPower;

        lighting = diffuse + (spec * lightColor * attenuation * spotFactor);
    }

    return float4(lighting, 1.0f);
}
// PostProcess.hlsl
// Пост-обработка: операции над уже готовой картинкой сцены.
// На входе картинка после lighting pass (в HDR, яркость может быть больше 1) и весь G-буфер,
// на выходе картинка для экрана.
//
// Проходы:
//  BrightPS - оставляет только очень яркие пиксели (первый шаг bloom)
//  BlurHPS  - размытие по Гауссу по горизонтали
//  BlurVPS  - размытие по Гауссу по вертикали
//  FinalPS  - собирает итог: сцена + свечение, хроматическая аберрация, режимы просмотра G-буфера
// Все проходы рисуют один и тот же прямоугольник на весь экран из PostVS.

// Настройки, которые приходят из C++ каждый кадр (root constants, 4 числа)
cbuffer cbPost : register(b0)
{
    uint gBloomEnabled; // 1 - bloom включён (клавиша B)
    uint gChromaEnabled; // 1 - хроматическая аберрация включена (клавиша N)
    uint gViewMode; // что показывать (клавиша G): 0 итог, 1 альбедо, 2 нормали, 3 позиции, 4 только свечение
    uint gPad; // не используется, просто дополняет до 4 чисел
};

// Картинка сцены и две текстуры для bloom (в них по очереди пишется и читается размытие)
Texture2D gSceneTex : register(t0);
Texture2D gBloomTexA : register(t1);
Texture2D gBloomTexB : register(t2);

// G-буфер (те же текстуры, что читает lighting pass)
Texture2D<float4> gAlbedoTex : register(t3);
Texture2D<float4> gWorldPosTex : register(t4);
Texture2D<float4> gNormalTex : register(t5);

// линейная фильтрация, на краях текстуры берётся крайний пиксель (clamp)
SamplerState gLinearClamp : register(s0);

// Параметры эффектов. Их можно менять прямо здесь, C++ пересобирать не нужно
static const float kBloomThreshold = 1.0f; // пиксель светится, если его яркость больше этого числа
static const float kBloomIntensity = 0.6f; // с каким весом свечение прибавляется к сцене
static const float kBlurStep = 2.0f; // через сколько текселей брать соседей при размытии
static const float kChromaStrength = 0.3f; // насколько сильно разъезжаются красный и синий к краям экрана

struct PostVSOut
{
    float4 PosH : SV_POSITION;
    float2 TexC : TEXCOORD;
};

// Вершинный шейдер full-screen quad без вершинного буфера.
// C++ вызывает DrawInstanced(4, 1, 0, 0) с топологией TRIANGLESTRIP,
// и шейдер сам получает номер вершины id = 0, 1, 2, 3.
// Из битов номера получаются UV углов экрана:
//  id = 0 -> (0, 0) левый верхний
//  id = 1 -> (1, 0) правый верхний
//  id = 2 -> (0, 1) левый нижний
//  id = 3 -> (1, 1) правый нижний
// Потом UV [0,1] переводится в координаты экрана NDC [-1,1], ось Y переворачивается (у текстуры Y вниз, у экрана вверх).
// Strip из 4 вершин - это 2 треугольника (0,1,2) и (1,2,3), вместе прямоугольник на весь экран
PostVSOut PostVS(uint id : SV_VertexID)
{
    PostVSOut vout;
    vout.TexC = float2(id & 1, (id & 2) >> 1);
    vout.PosH = float4(vout.TexC * float2(2, -2) + float2(-1, 1), 0, 1);
    return vout;
}

// BLOOM, шаг 1: выделение ярких пикселей.
// Яркость (luminance) - это взвешенная сумма R, G, B: глаз сильнее всего чувствует зелёный, слабее всего синий.
// Если пиксель ярче порога, он остаётся как есть, иначе становится чёрным.
// Рисуется в текстуру вдвое меньше экрана, линейная фильтрация сама усредняет 4 пикселя в 1
float4 BrightPS(PostVSOut pin) : SV_Target
{
    float3 color = gSceneTex.Sample(gLinearClamp, pin.TexC).rgb;
    float luminance = dot(color, float3(0.2126f, 0.7152f, 0.0722f));
    float3 bloomColor = luminance > kBloomThreshold ? color : float3(0, 0, 0);
    return float4(bloomColor, 1.0f);
}

// BLOOM, шаг 2: размытие по Гауссу.
// Каждый пиксель = взвешенная сумма самого пикселя и 3 соседей с каждой стороны.
// Веса взяты из функции Гаусса: чем дальше сосед, тем меньше его вклад. В сумме веса дают 1,
// поэтому общая яркость картинки не меняется.
// 2D размытие делится на два прохода: сначала по горизонтали, потом по вертикали.
// Так на пиксель нужно 7 + 7 выборок вместо 7 * 7 = 49
static const float kBlurWeights[4] = { 0.2161f, 0.1907f, 0.1311f, 0.0702f };

// dir - направление размытия: (1, 0) горизонталь, (0, 1) вертикаль
float4 Blur(Texture2D source, float2 uv, float2 dir)
{
    // размер одного текселя в UV: 1 / ширина и 1 / высота текстуры
    float width, height;
    source.GetDimensions(width, height);
    float2 texelSize = dir * float2(1.0f / width, 1.0f / height) * kBlurStep;

    float4 color = source.SampleLevel(gLinearClamp, uv, 0) * kBlurWeights[0];

    [unroll]
    for (int i = 1; i <= 3; i++)
    {
        color += source.SampleLevel(gLinearClamp, uv + texelSize * i, 0) * kBlurWeights[i];
        color += source.SampleLevel(gLinearClamp, uv - texelSize * i, 0) * kBlurWeights[i];
    }
    return color;
}

// горизонтальный проход: читает A, C++ направляет результат в B
float4 BlurHPS(PostVSOut pin) : SV_Target
{
    return Blur(gBloomTexA, pin.TexC, float2(1, 0));
}

// вертикальный проход: читает B, C++ направляет результат обратно в A
float4 BlurVPS(PostVSOut pin) : SV_Target
{
    return Blur(gBloomTexB, pin.TexC, float2(0, 1));
}

// Цвет сцены в точке uv вместе со свечением (если bloom включён).
// BLOOM, шаг 3: размытые яркие пятна прибавляются к сцене с весом kBloomIntensity,
// и свет как бы "растекается" от ярких мест на соседние тёмные
float3 SceneColor(float2 uv)
{
    float3 color = gSceneTex.Sample(gLinearClamp, uv).rgb;
    if (gBloomEnabled != 0)
        color += gBloomTexA.Sample(gLinearClamp, uv).rgb * kBloomIntensity;
    return color;
}

// Итоговый проход, рисует в back buffer.
// Здесь же пиксельный шейдер читает текстуры G-буфера: по клавише G можно посмотреть, что в них лежит
float4 FinalPS(PostVSOut pin) : SV_Target
{
    // координаты пикселя на экране, чтобы читать G-буфер через Load, как в lighting pass
    int3 pixel = int3((int2) pin.PosH.xy, 0);

    // 1 - альбедо (цвет текстуры без освещения)
    if (gViewMode == 1)
        return float4(gAlbedoTex.Load(pixel).rgb, 1.0f);

    // 2 - нормали: вектор [-1,1] переводим в цвет [0,1]. Смотрит по X - красный, по Y - зелёный, по Z - синий
    if (gViewMode == 2)
    {
        float3 n = gNormalTex.Load(pixel).xyz;
        if (dot(n, n) < 0.0001f)
            return float4(0, 0, 0, 1); // пустой пиксель (небо), нормали нет
        return float4(normalize(n) * 0.5f + 0.5f, 1.0f);
    }

    // 3 - мировые позиции: frac оставляет дробную часть, поэтому цвет повторяется каждые 250 единиц
    // и на стенах видны цветные полосы, как координатная сетка
    if (gViewMode == 3)
    {
        float3 p = gWorldPosTex.Load(pixel).xyz;
        return float4(frac(p / 250.0f), 1.0f);
    }

    // 4 - только свечение из bloom, без сцены: видно, какие места посчитались яркими
    if (gViewMode == 4)
        return float4(saturate(gBloomTexA.Sample(gLinearClamp, pin.TexC).rgb), 1.0f);

    // 0 - обычная картинка
    float2 uv = pin.TexC;
    float3 color;

    if (gChromaEnabled != 0)
    {
        // ХРОМАТИЧЕСКАЯ АБЕРРАЦИЯ.
        // Дешёвый объектив по-разному преломляет разные цвета, и на краях кадра они немного расходятся.
        // Имитируем так: красный канал читаем чуть дальше от центра экрана, синий - чуть ближе, зелёный - на месте.
        // dir - вектор от центра экрана (0.5, 0.5) к пикселю: в центре он нулевой, к краям растёт,
        // поэтому в центре эффекта нет, а по краям появляется цветная кайма
        float2 dir = uv - float2(0.5f, 0.5f);
        color.r = SceneColor(uv + dir * kChromaStrength).r;
        color.g = SceneColor(uv).g;
        color.b = SceneColor(uv - dir * kChromaStrength).b;
    }
    else
    {
        color = SceneColor(uv);
    }

    // back buffer хранит только [0,1], всё что ярче обрезается до 1
    return float4(saturate(color), 1.0f);
}
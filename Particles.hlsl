// Particles.hlsl
// Система частиц целиком на GPU.
//
// Буферы:
//  gParticles  - пул частиц (Particle Pool): позиция, скорость, возраст, цвет и т.д.
//  Dead List   - номера свободных (мёртвых) ячеек пула. Со счётчиком:
//                EmitCS берёт из него номера через Consume(), SimulateCS кладёт обратно через Append()
//  Alive List  - номера живых частиц этого кадра. SimulateCS добавляет их через Append(),
//                а вершинный шейдер читает их при отрисовке
//
// Кадр:
//  EmitCS      - рождает новые частицы в свободных ячейках (Consume из Dead List)
//  SimulateCS  - двигает живые частицы, умершие возвращает в Dead List, живые складывает в Alive List
//  VS -> GS -> PS - рисование: одна точка на частицу, GS разворачивает её в билборд из 4 вершин,
//                PS пишет непрозрачную круглую частицу прямо в G-buffer, поэтому её освещает
//                обычный lighting pass вместе с тенями

// Одна частица. Раскладка должна совпадать со struct GpuParticle в ParticleSystem.h
struct Particle
{
    float3 Position; // текущее положение в мире
    float Age; // сколько секунд частица уже живёт
    float3 Velocity; // скорость (направление и величина)
    float LifeSpan; // сколько секунд частица должна прожить
    float4 StartColor; // цвет при рождении
    float4 EndColor; // цвет в конце жизни, между ними цвет плавно меняется
    float StartSize; // радиус при рождении
    float EndSize; // радиус в конце жизни
    float Weight; // насколько на частицу действует гравитация
    uint Alive; // 1 - частица живая, 0 - ячейка свободна
};


// COMPUTE: EMIT и SIMULATE

// Параметры симуляции на этот кадр (раскладка = ParticleSimConstants в ParticleSystem.h)
cbuffer cbParticleSim : register(b0)
{
    float3 gEmitterPos; // откуда вылетают частицы
    float gDeltaTime; // шаг времени этого кадра
    float3 gGravity; // ускорение свободного падения
    uint gEmitCount; // сколько частиц родить в этом кадре
    float3 gWind; // постоянный ветер, тоже ускорение
    uint gRandomSeed; // зерно случайных чисел, своё на каждый кадр
    float gGroundHeight; // ниже этой высоты частица "ударилась о землю" и умирает
    uint gMaxParticles; // размер пула
    float2 gSimPad;
};

// Сколько свободных ячеек сейчас в Dead List. Это значение счётчика Dead List,
// скопированное на GPU перед EmitCS, чтобы не забрать из списка больше, чем в нём есть
cbuffer cbDeadCount : register(b1)
{
    uint gDeadCount;
    uint3 gDeadPad;
};

RWStructuredBuffer<Particle> gParticles : register(u0);

// Один и тот же буфер Dead List со счётчиком: в EmitCS он виден как Consume, в SimulateCS - как Append
ConsumeStructuredBuffer<uint> gDeadListConsume : register(u1);
AppendStructuredBuffer<uint> gDeadListAppend : register(u1);

AppendStructuredBuffer<uint> gAliveList : register(u2);

// Хеш-функция Ванга: из любого числа делает "случайное" число
uint WangHash(uint seed)
{
    seed = (seed ^ 61u) ^ (seed >> 16);
    seed *= 9u;
    seed = seed ^ (seed >> 4);
    seed *= 0x27d4eb2du;
    seed = seed ^ (seed >> 15);
    return seed;
}

// случайное число от 0 до 1, состояние обновляется при каждом вызове
float Rand(inout uint state)
{
    state = WangHash(state);
    return (float) (state & 0x00FFFFFFu) / 16777216.0f;
}

// Параметры эмиттера. Каждый атрибут новой частицы берётся случайно в своём диапазоне
static const float kSpeedMin = 400.0f; // скорость вылета вверх
static const float kSpeedMax = 600.0f;
static const float kSpread = 0.3f; // насколько струя расходится в стороны
static const float kLifeMin = 3.0f; // время жизни в секундах
static const float kLifeMax = 5.0f;
static const float kSizeMin = 3.0f; // радиус частицы
static const float kSizeMax = 6.0f;

// Emit: один поток рождает одну частицу.
// Поток берёт свободный номер из Dead List (Consume) и заполняет эту ячейку пула начальными значениями
[numthreads(256, 1, 1)]
void EmitCS(uint3 id : SV_DispatchThreadID)
{
    // лишние потоки последней группы, и не больше, чем свободных ячеек, иначе счётчик уйдёт в минус
    if (id.x >= gEmitCount || id.x >= gDeadCount)
        return;

    uint index = gDeadListConsume.Consume();

    // у каждого потока своё зерно: номер потока плюс зерно кадра
    uint rng = WangHash(id.x * 1973u + gRandomSeed * 9277u + 1u);

    // направление: вверх с небольшим случайным отклонением в стороны
    float3 dir = normalize(float3((Rand(rng) * 2.0f - 1.0f) * kSpread, 1.0f, (Rand(rng) * 2.0f - 1.0f) * kSpread));
    float speed = lerp(kSpeedMin, kSpeedMax, Rand(rng));

    Particle p;
    p.Position = gEmitterPos;
    p.Age = 0.0f;
    p.Velocity = dir * speed;
    p.LifeSpan = lerp(kLifeMin, kLifeMax, Rand(rng));

    // тёплый жёлто-оранжевый цвет при рождении, тёмно-красный к концу жизни, с небольшим разбросом
    float tint = Rand(rng);
    p.StartColor = float4(1.0f, lerp(0.6f, 0.95f, tint), lerp(0.1f, 0.4f, tint), 1.0f);
    p.EndColor = float4(lerp(0.5f, 0.8f, tint), 0.05f, 0.02f, 1.0f);

    float size = lerp(kSizeMin, kSizeMax, Rand(rng));
    p.StartSize = size;
    p.EndSize = size * 0.3f; // к концу жизни частица уменьшается
    p.Weight = lerp(0.8f, 1.2f, Rand(rng));
    p.Alive = 1;

    gParticles[index] = p;
}

// Simulate: один поток на одну ячейку пула.
// Живую частицу двигает интегрированием Эйлера: силы дают ускорение, ускорение меняет скорость,
// скорость меняет положение. Умершую возвращает в Dead List, живую добавляет в Alive List
[numthreads(256, 1, 1)]
void SimulateCS(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gMaxParticles)
        return;

    Particle p = gParticles[id.x];
    if (p.Alive == 0)
        return;

    p.Age += gDeltaTime;

    // a = сумма сил / масса. Гравитация умножается на вес, ветер действует одинаково на всех
    float3 acceleration = gGravity * p.Weight + gWind;

    // v = v + a * dt, s = s + v * dt
    p.Velocity += acceleration * gDeltaTime;
    p.Position += p.Velocity * gDeltaTime;

    // смерть: кончилось время жизни или частица упала на землю
    if (p.Age >= p.LifeSpan || p.Position.y < gGroundHeight)
    {
        p.Alive = 0;
        gDeadListAppend.Append(id.x);
    }
    else
    {
        gAliveList.Append(id.x);
    }

    gParticles[id.x] = p;
}


// RENDER: VS -> GS -> PS

// Камера (раскладка = ParticleRenderConstants в ParticleSystem.h)
cbuffer cbParticleRender : register(b0)
{
    float4x4 gView;
    float4x4 gProj;
    float4x4 gInvView; // из view space обратно в мир, для позиции и нормали в G-buffer
    float gCurvature; // 0 - плоская нормаль билборда, 1 - нормаль как у шара
    float3 gRenderPad;
};

// при рисовании буферы только читаются
StructuredBuffer<Particle> gParticlesRead : register(t0);
StructuredBuffer<uint> gAliveListRead : register(t1);

struct ParticleVSOut
{
    float3 PosW : POSITION;
    float Size : SIZE;
    float4 Color : COLOR;
};

struct ParticleGSOut
{
    float4 PosH : SV_POSITION;
    float3 PosW : POSITION1;
    float2 Corner : TEXCOORD0; // координаты внутри билборда от -1 до 1
    float4 Color : COLOR;
};

// Вершинный шейдер. Вершинного буфера нет: номер вершины = номер в Alive List,
// по нему берём номер ячейки пула и читаем частицу
ParticleVSOut ParticleVS(uint vertexId : SV_VertexID)
{
    uint index = gAliveListRead[vertexId];
    Particle p = gParticlesRead[index];

    // цвет и размер меняются со временем: линейно от начальных к конечным
    float t = saturate(p.Age / p.LifeSpan);

    ParticleVSOut vout;
    vout.PosW = p.Position;
    vout.Size = lerp(p.StartSize, p.EndSize, t);
    vout.Color = lerp(p.StartColor, p.EndColor, t);
    return vout;
}

// Geometry shader: одна точка превращается в квадрат из 4 вершин (два треугольника strip'ом).
// Квадрат строится в view space: там оси X и Y всегда параллельны экрану,
// поэтому билборд автоматически смотрит на камеру
[maxvertexcount(4)]
void ParticleGS(point ParticleVSOut gin[1], inout TriangleStream<ParticleGSOut> stream)
{
    float3 centerV = mul(float4(gin[0].PosW, 1.0f), gView).xyz;

    // порядок углов для triangle strip: левый верх, правый верх, левый низ, правый низ
    static const float2 corners[4] =
    {
        float2(-1.0f, 1.0f),
        float2(1.0f, 1.0f),
        float2(-1.0f, -1.0f),
        float2(1.0f, -1.0f)
    };

    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        float3 posV = centerV + float3(corners[i] * gin[0].Size, 0.0f);

        ParticleGSOut gout;
        gout.PosH = mul(float4(posV, 1.0f), gProj);
        gout.PosW = mul(float4(posV, 1.0f), gInvView).xyz;
        gout.Corner = corners[i];
        gout.Color = gin[0].Color;
        stream.Append(gout);
    }
}

// то же, что пишет geometry pass сцены: 3 рендертаргета G-буфера
struct Gbuffer
{
    float4 DiffuseSpec : SV_Target0;
    float4 WorldPos : SV_Target1;
    float4 Normal : SV_Target2;
};

// Пиксельный шейдер: непрозрачная круглая частица.
// Всё, что за пределами круга, отбрасывается через clip - блендинг и сортировка не нужны.
// Здесь нельзя ставить [earlydepthstencil], как в шейдере сцены: тогда глубина записалась бы
// до clip, и отброшенные углы квадрата всё равно закрывали бы то, что за ними.
// Нормаль приближаем по слайду про освещение частиц: смесь нормали билборда (к камере)
// и направления от центра к краю, поэтому частица освещается как выпуклая
Gbuffer ParticlePS(ParticleGSOut pin)
{
    float r2 = dot(pin.Corner, pin.Corner);
    clip(1.0f - r2);

    // в view space камера смотрит вдоль +Z, значит нормаль, направленная на камеру, - это (0,0,-1)
    float3 billboardNormal = float3(0.0f, 0.0f, -1.0f);
    float3 radial = float3(pin.Corner / max(sqrt(r2), 0.0001f), 0.0f);
    float3 normalV = normalize(lerp(billboardNormal, radial, gCurvature * sqrt(r2)));

    // нормаль из view space в мир, в G-buffer хранятся мировые нормали
    float3 normalW = normalize(mul(normalV, (float3x3) gInvView));

    Gbuffer gout;
    gout.DiffuseSpec = float4(pin.Color.rgb, 0.2f); // небольшая сила блика
    gout.WorldPos = float4(pin.PosW, 1.0f);
    gout.Normal = float4(normalW, 0.0f);
    return gout;
}
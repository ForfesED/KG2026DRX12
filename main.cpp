
#include <windows.h>
#include <wrl/client.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <wincodec.h>

#include <string>
#include <vector>
#include <unordered_map>
#include <map>
#include <random>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <cmath>
#include <cfloat>

#include "Gbuffer.h"
#include "RenderingSystem.h"
#include "Culling.h"

// тут подключаем нужные библиотеки, чтобы линкер не ругался
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

using namespace DirectX;
using namespace Microsoft::WRL;

// размер окна
const UINT kClientWidth = 1024;
const UINT kClientHeight = 768;
const UINT kFrameCount = 2; // количество бэк-буферов свапчейна


inline void ThrowIfFailed(HRESULT hr, const char* what)
{
    if (FAILED(hr))
    {
        char buf[256];
        sprintf_s(buf, "Ошибка: %s (HRESULT = 0x%08X)", what, (unsigned int)hr);
        throw std::runtime_error(buf);
    }
}

// константные буферы в D3D12 должны быть выровнены по 256 байт,
// эта функция просто округляет размер вверх до 256
inline UINT CalcCBSize(UINT byteSize)
{
    return (byteSize + 255) & ~255;
}


// Вершина модели: позиция, нормаль, текстурные координаты и базис tangent space.
// Tangent и Binormal вместе с нормалью задают оси, в которых записана карта нормалей.
struct Vertex
{
    XMFLOAT3 Pos;
    XMFLOAT3 Normal;
    XMFLOAT2 TexC;
    XMFLOAT3 Tangent;
    XMFLOAT3 Binormal;
    XMFLOAT3 DispNormal; // направление сдвига при displacement, общее для всех копий вершины в этой точке
    float    DispWeight; // 1 - вершину можно сдвигать, 0 - вершина на шве UV, сдвигать нельзя
};
// Константный буфер b0 - данные на каждый объект (сабмеш)
struct ObjectConstants
{
    XMFLOAT4X4 World;
    XMFLOAT4X4 TexTransform; // тайлинг и анимация текстуры
    XMFLOAT4   DiffuseAlbedo;
    XMFLOAT4X4 ReliefTexTransform; // только тайлинг, для карт нормалей и высот
};

// Константный буфер b1 - общие данные кадра.
// Раскладка должна совпадать с cbPass в Shaders.hlsl, поля сгруппированы по 16 байт.
struct PassConstants
{
    XMFLOAT4X4 ViewProj;

    XMFLOAT3 EyePosW;           // позиция камеры, по ней hull shader считает расстояние
    float    TessMinDist;       // ближе этого расстояния тесселяция максимальная

    float    TessMaxDist;       // дальше этого расстояния тесселяция минимальная
    float    TessMinFactor;     // коэффициент тесселяции вдали
    float    TessMaxFactor;     // коэффициент тесселяции вблизи
    float    DisplacementScale; // высота смещения для белого пикселя displacement-карты

    float    DisplacementBias;  // постоянная добавка к смещению
    XMFLOAT3 Pad;
};

// Информация о материале, которую мы вытащили из .mtl файла
struct MaterialData
{
    std::string Name;
    XMFLOAT4 DiffuseAlbedo = { 1.0f, 1.0f, 1.0f, 1.0f }; // цвет Kd
    std::string DiffuseMapFile;      // текстура цвета (map_Kd), может быть пустой
    std::string NormalMapFile;       // карта нормалей (norm, map_Kn или map_Bump с нормалями)
    std::string DisplacementMapFile; // карта высот для тесселяции (disp, map_Disp или map_Bump с высотами)

    // Тайлинг и анимация - в обычном .mtl таких полей нет, поэтому
    // мы их просто выставляем в коде (см. функцию SetupMaterialAnimation)
    XMFLOAT2 TileScale = { 1.0f, 1.0f };
    XMFLOAT2 ScrollSpeed = { 0.0f, 0.0f };

    // Первый из трёх дескрипторов материала в SRV куче: diffuse, normal, displacement подряд.
    // 0 - это тройка текстур по умолчанию (белая, плоская нормаль, чёрная)
    int SrvIndex = 0;
};

// Один "кусок" модели, который рисуется одним вызовом DrawIndexed
// и использует один материал
struct Submesh
{
    UINT IndexCount = 0;
    UINT StartIndexLocation = 0;
    int MaterialIndex = -1;
};



// маленький помощник - грузит .mtl файл и возвращает список материалов + теперь читает карту высот и нормалей
// переводит строку в нижний регистр, в .mtl ключевые слова пишут кто как хочет (map_Kd, map_kd, MAP_KD)
static std::string ToLower(std::string str)
{
    for (char& c : str)
        c = (char)tolower((unsigned char)c);
    return str;
}

// map_Bump в разных моделях означает разное: в одних это карта нормалей, в других карта высот.
// Отличаем по имени файла - у карт нормалей обычно есть normal, nrm, ddn или суффикс _n
static bool LooksLikeNormalMap(const std::string& fileName)
{
    std::string f = ToLower(fileName);
    return f.find("normal") != std::string::npos ||
        f.find("nrm") != std::string::npos ||
        f.find("ddn") != std::string::npos ||
        f.find("_norm") != std::string::npos ||
        f.find("_n.") != std::string::npos;
}

// грузит .mtl файл и возвращает список материалов вместе с картами нормалей и высот
std::vector<MaterialData> LoadMtl(const std::string& path)
{
    std::vector<MaterialData> materials;
    std::ifstream file(path);
    if (!file.is_open())
        return materials; // если файла нет - вернём пустой список, не страшно

    std::string line;
    while (std::getline(file, line))
    {
        std::istringstream ss(line);
        std::string token;
        ss >> token;
        std::string key = ToLower(token);

        // имя файла берём последним словом строки, т.к. перед ним могут стоять опции вроде "-bm 1.0"
        auto ReadFileName = [&ss]() -> std::string
            {
                std::string word, last;
                while (ss >> word)
                    last = word;
                return last;
            };

        if (key == "newmtl")
        {
            MaterialData m;
            ss >> m.Name;
            materials.push_back(m);
        }
        else if (materials.empty())
        {
            // пока не встретили ни одного newmtl, остальные строки относить некуда
            continue;
        }
        else if (key == "kd")
        {
            float r, g, b;
            ss >> r >> g >> b;
            materials.back().DiffuseAlbedo = XMFLOAT4(r, g, b, 1.0f);
        }
        else if (key == "map_kd")
        {
            materials.back().DiffuseMapFile = ReadFileName();
        }
        else if (key == "norm" || key == "map_kn" || key == "map_normal")
        {
            materials.back().NormalMapFile = ReadFileName();
        }
        else if (key == "disp" || key == "map_disp")
        {
            materials.back().DisplacementMapFile = ReadFileName();
        }
        else if (key == "map_bump" || key == "bump")
        {
            // не перезаписываем то, что уже задано явными norm/disp
            std::string fileName = ReadFileName();
            MaterialData& m = materials.back();
            if (LooksLikeNormalMap(fileName))
            {
                if (m.NormalMapFile.empty()) m.NormalMapFile = fileName;
            }
            else
            {
                if (m.DisplacementMapFile.empty()) m.DisplacementMapFile = fileName;
            }
        }
    }
    return materials;
}

// загрузка модели из .obj. folder - папка, где лежит .obj и всё что он подключает
struct LoadedModel
{
    std::vector<Vertex> Vertices;
    std::vector<uint32_t> Indices;
    std::vector<Submesh> Submeshes;
    std::vector<MaterialData> Materials;
};

LoadedModel LoadObjModel(const std::string& folder, const std::string& objFileName)
{
    LoadedModel model;

    std::ifstream file(folder + "\\" + objFileName);
    if (!file.is_open())
        throw std::runtime_error("Не получилось открыть " + objFileName + " - положи модель в папку obj!");

    // тут временно копим то, что прочитали из файла
    std::vector<XMFLOAT3> positions;
    std::vector<XMFLOAT3> normals;
    std::vector<XMFLOAT2> texcoords;

    // т.к. в obj у каждого индекса грани свой набор (позиция/uv/нормаль),
    // а нам нужна "одна" вершина со всеми данными сразу - делаем кэш уникальных вершин
    std::unordered_map<std::string, uint32_t> uniqueVerts;

    int currentMaterial = -1;

    // лямбда, которая по строке вида "5/2/1" находит/создаёт вершину и
    // возвращает её индекс в итоговом массиве Vertices
    auto GetVertexIndex = [&](const std::string& token) -> uint32_t
    {
        auto it = uniqueVerts.find(token);
        if (it != uniqueVerts.end())
            return it->second;

        // разбираем строку "posIdx/texIdx/normIdx" (texIdx или normIdx могут отсутствовать)
        int posIdx = 0, texIdx = 0, normIdx = 0;
        size_t firstSlash = token.find('/');
        if (firstSlash == std::string::npos)
        {
            posIdx = std::stoi(token);
        }
        else
        {
            posIdx = std::stoi(token.substr(0, firstSlash));
            size_t secondSlash = token.find('/', firstSlash + 1);
            if (secondSlash == std::string::npos)
            {
                // вида "v//" не бывает на практике без второго слэша, но всё равно проверим
                std::string texPart = token.substr(firstSlash + 1);
                if (!texPart.empty()) texIdx = std::stoi(texPart);
            }
            else
            {
                std::string texPart = token.substr(firstSlash + 1, secondSlash - firstSlash - 1);
                if (!texPart.empty()) texIdx = std::stoi(texPart);

                std::string normPart = token.substr(secondSlash + 1);
                if (!normPart.empty()) normIdx = std::stoi(normPart);
            }
        }

        Vertex v = {};
        // индексы в obj начинаются с 1, поэтому -1
        v.Pos = (posIdx > 0) ? positions[posIdx - 1] : XMFLOAT3(0, 0, 0);
        v.TexC = (texIdx > 0) ? texcoords[texIdx - 1] : XMFLOAT2(0, 0);
        v.Normal = (normIdx > 0) ? normals[normIdx - 1] : XMFLOAT3(0, 1, 0);

        uint32_t newIndex = (uint32_t)model.Vertices.size();
        model.Vertices.push_back(v);
        uniqueVerts[token] = newIndex;
        return newIndex;
    };

    std::string line;
    while (std::getline(file, line))
    {
        std::istringstream ss(line);
        std::string token;
        ss >> token;

        if (token == "v")
        {
            float x, y, z;
            ss >> x >> y >> z;
            positions.push_back(XMFLOAT3(x, y, z));
        }
        else if (token == "vn")
        {
            float x, y, z;
            ss >> x >> y >> z;
            normals.push_back(XMFLOAT3(x, y, z));
        }
        else if (token == "vt")
        {
            float u, v;
            ss >> u >> v;
            // в OBJ ось V обычно идёт снизу вверх, а в текстурах сверху вниз,
            // поэтому многие модели уже учитывают это сами. Если текстура
            // окажется "вверх ногами" - попробуй раскомментировать строку ниже:
            // v = 1.0f - v;
            texcoords.push_back(XMFLOAT2(u, v));
        }
        else if (token == "mtllib")
        {
            std::string mtlName;
            ss >> mtlName;
            model.Materials = LoadMtl(folder + "\\" + mtlName);
        }
        else if (token == "usemtl")
        {
            std::string matName;
            ss >> matName;
            currentMaterial = -1;
            for (size_t i = 0; i < model.Materials.size(); ++i)
            {
                if (model.Materials[i].Name == matName)
                {
                    currentMaterial = (int)i;
                    break;
                }
            }

            // начинаем новый сабмеш для этого материала
            Submesh sm;
            sm.MaterialIndex = currentMaterial;
            sm.StartIndexLocation = (UINT)model.Indices.size();
            model.Submeshes.push_back(sm);
        }
        else if (token == "f")
        {
            // если ни одного usemtl не было - создадим сабмеш без материала
            if (model.Submeshes.empty())
            {
                Submesh sm;
                sm.MaterialIndex = -1;
                sm.StartIndexLocation = 0;
                model.Submeshes.push_back(sm);
            }

            // читаем все вершины этой грани (может быть 3, 4 и больше - полигон)
            std::vector<uint32_t> faceIndices;
            std::string vertToken;
            while (ss >> vertToken)
                faceIndices.push_back(GetVertexIndex(vertToken));

            // режем полигон на треугольники "веером" от первой вершины
            for (size_t i = 1; i + 1 < faceIndices.size(); ++i)
            {
                model.Indices.push_back(faceIndices[0]);
                model.Indices.push_back(faceIndices[i]);
                model.Indices.push_back(faceIndices[i + 1]);
            }
        }
        // остальные токены (комментарии "#", "s" - сглаживание, и т.д.) просто игнорируем
    }

    // считаем сколько индексов попало в каждый сабмеш
    for (size_t i = 0; i < model.Submeshes.size(); ++i)
    {
        UINT endIndex = (i + 1 < model.Submeshes.size())
            ? model.Submeshes[i + 1].StartIndexLocation
            : (UINT)model.Indices.size();
        model.Submeshes[i].IndexCount = endIndex - model.Submeshes[i].StartIndexLocation;
    }

    return model;
}

// Считает Tangent и Binormal для каждой вершины.
// Для треугольника p0 p1 p2 с UV uv0 uv1 uv2 рёбра выражаются через T и B:
//   edge1 = du1 * T + dv1 * B
//   edge2 = du2 * T + dv2 * B
// Это система 2x2, решаем её через обратную матрицу (f = 1 / определитель).
// Вершина может входить в несколько треугольников, поэтому векторы копим суммой,
// а в конце нормализуем и делаем T перпендикулярным нормали.
void ComputeTangents(LoadedModel& model)// расчёт тангентов
{
    const size_t vertexCount = model.Vertices.size();
    std::vector<XMVECTOR> tangents(vertexCount, XMVectorZero());
    std::vector<XMVECTOR> binormals(vertexCount, XMVectorZero());

    for (size_t i = 0; i + 2 < model.Indices.size(); i += 3)
    {
        uint32_t i0 = model.Indices[i];
        uint32_t i1 = model.Indices[i + 1];
        uint32_t i2 = model.Indices[i + 2];

        const Vertex& v0 = model.Vertices[i0];
        const Vertex& v1 = model.Vertices[i1];
        const Vertex& v2 = model.Vertices[i2];

        XMVECTOR p0 = XMLoadFloat3(&v0.Pos);
        XMVECTOR edge1 = XMVectorSubtract(XMLoadFloat3(&v1.Pos), p0);
        XMVECTOR edge2 = XMVectorSubtract(XMLoadFloat3(&v2.Pos), p0);

        float du1 = v1.TexC.x - v0.TexC.x;
        float dv1 = v1.TexC.y - v0.TexC.y;
        float du2 = v2.TexC.x - v0.TexC.x;
        float dv2 = v2.TexC.y - v0.TexC.y;

        // если UV у треугольника вырождены (все в одной точке или на одной линии), решения нет
        float det = du1 * dv2 - du2 * dv1;
        if (fabsf(det) < 1e-12f)
            continue;
        float f = 1.0f / det;

        // T = f * (dv2 * edge1 - dv1 * edge2)
        XMVECTOR t = XMVectorScale(XMVectorSubtract(XMVectorScale(edge1, dv2), XMVectorScale(edge2, dv1)), f);
        // B = f * (-du2 * edge1 + du1 * edge2)
        XMVECTOR b = XMVectorScale(XMVectorSubtract(XMVectorScale(edge2, du1), XMVectorScale(edge1, du2)), f);

        tangents[i0] = XMVectorAdd(tangents[i0], t);
        tangents[i1] = XMVectorAdd(tangents[i1], t);
        tangents[i2] = XMVectorAdd(tangents[i2], t);

        binormals[i0] = XMVectorAdd(binormals[i0], b);
        binormals[i1] = XMVectorAdd(binormals[i1], b);
        binormals[i2] = XMVectorAdd(binormals[i2], b);
    }

    for (size_t i = 0; i < vertexCount; ++i)
    {
        Vertex& v = model.Vertices[i];
        XMVECTOR n = XMVector3Normalize(XMLoadFloat3(&v.Normal));
        XMVECTOR t = tangents[i];

        // Грам-Шмидт: убираем из T проекцию на нормаль, чтобы T лежал в плоскости поверхности
        t = XMVectorSubtract(t, XMVectorScale(n, XMVectorGetX(XMVector3Dot(n, t))));

        // у вершины без нормальных UV тангента нет, берём любой вектор, перпендикулярный нормали
        if (XMVectorGetX(XMVector3LengthSq(t)) < 1e-12f)
        {
            XMVECTOR helper = (fabsf(v.Normal.y) < 0.99f) ? XMVectorSet(0, 1, 0, 0) : XMVectorSet(1, 0, 0, 0);
            t = XMVector3Cross(helper, n);
        }
        t = XMVector3Normalize(t);

        // бинормаль достраиваем как перпендикуляр к N и T, а сторону берём от посчитанной по UV,
        // так правильно обрабатываются зеркально развёрнутые UV
        XMVECTOR b = XMVector3Cross(n, t);
        if (XMVectorGetX(XMVector3Dot(b, binormals[i])) < 0.0f)
            b = XMVectorNegate(b);

        XMStoreFloat3(&v.Tangent, t);
        XMStoreFloat3(&v.Binormal, b);
    }
}
// Готовит данные для displacement без разрывов.
// В .obj одна и та же точка часто представлена несколькими вершинами:
//  на острых рёбрах (угол колонны) у копий разные нормали,
//  на швах развёртки у копий разные UV.
// Если сдвигать каждую копию вдоль своей нормали и по своей высоте из текстуры,
// копии разъезжаются и между гранями появляется щель.
// Решение:
//  все копии одной точки сдвигаем вдоль одной общей, усреднённой нормали
//  если у копий разные UV (шов), высоты у них всё равно разные, поэтому такие вершины не сдвигаем вообще,
//  а внутри треугольника сдвиг плавно нарастает от 0 на шве до полного
void ComputeDisplacementData(LoadedModel& model)
{
    // ключ для группировки вершин по позиции
    struct PosKey
    {
        float x, y, z;
        bool operator<(const PosKey& o) const
        {
            if (x != o.x) return x < o.x;
            if (y != o.y) return y < o.y;
            return z < o.z;
        }
    };

    // собираем индексы всех вершин, у которых совпадает позиция
    std::map<PosKey, std::vector<uint32_t>> groups;
    for (uint32_t i = 0; i < (uint32_t)model.Vertices.size(); ++i)
    {
        const XMFLOAT3& p = model.Vertices[i].Pos;
        groups[{ p.x, p.y, p.z }].push_back(i);
    }

    for (auto& group : groups)
    {
        const std::vector<uint32_t>& ids = group.second;
        const Vertex& first = model.Vertices[ids[0]];

        // сумма нормалей всех копий и проверка, что UV у всех копий одинаковые
        XMVECTOR sum = XMVectorZero();
        bool sameUV = true;
        for (uint32_t id : ids)
        {
            const Vertex& v = model.Vertices[id];
            sum = XMVectorAdd(sum, XMLoadFloat3(&v.Normal));

            if (fabsf(v.TexC.x - first.TexC.x) > 1e-5f || fabsf(v.TexC.y - first.TexC.y) > 1e-5f)
                sameUV = false;
        }

        // если нормали копий смотрят в противоположные стороны, сумма почти нулевая, тогда берём нормаль первой копии
        XMVECTOR avg = (XMVectorGetX(XMVector3LengthSq(sum)) > 1e-8f)
            ? XMVector3Normalize(sum)
            : XMVector3Normalize(XMLoadFloat3(&first.Normal));

        for (uint32_t id : ids)
        {
            XMStoreFloat3(&model.Vertices[id].DispNormal, avg);
            model.Vertices[id].DispWeight = sameUV ? 1.0f : 0.0f;
        }
    }
}


// Параметры сцены с кучей объектов.
// Каждая копия - целая Sponza (~262 тысячи треугольников), поэтому копий сотни, а не тысячи
const UINT kMassObjectCount = 200;        // сколько копий раскидать, если тормозит - уменьши
const float kMassSceneHalfSize = 4000.0f; // копии разбросаны по X и Z от -4000 до 4000
const float kMassSceneHeight = 1500.0f;   // и по Y от 0 до 1500

// Считает AABB модели в её собственных координатах: минимум и максимум по всем вершинам
AABB ComputeModelAABB(const LoadedModel& model)
{
    AABB box;
    box.Min = XMFLOAT3(FLT_MAX, FLT_MAX, FLT_MAX);
    box.Max = XMFLOAT3(-FLT_MAX, -FLT_MAX, -FLT_MAX);

    for (const Vertex& v : model.Vertices)
    {
        box.Min.x = fminf(box.Min.x, v.Pos.x);
        box.Min.y = fminf(box.Min.y, v.Pos.y);
        box.Min.z = fminf(box.Min.z, v.Pos.z);
        box.Max.x = fmaxf(box.Max.x, v.Pos.x);
        box.Max.y = fmaxf(box.Max.y, v.Pos.y);
        box.Max.z = fmaxf(box.Max.z, v.Pos.z);
    }
    return box;
}

// Параметры камеры, общие для проекции и для расчёта каскадов теней
const float kCameraFovY = XM_PIDIV4;
const float kCameraNear = 1.0f;
const float kCameraFar = 10000.0f;

// Параметры каскадных теней
const UINT kShadowMapSize = 2048;        // размер одной карты (одного каскада) в текселях
const float kShadowDistance = 3000.0f;   // до какой глубины от камеры есть тени, дальше всё освещено
const float kCascadeLambda = 0.8f;       // 0 - каскады равной длины, 1 - чисто логарифмическое деление

// Раскладка SRV кучи:
//  0, 1, 2 - текстуры по умолчанию: белая (diffuse), плоская нормаль, чёрная (нулевое смещение)
//  3 и дальше - по 3 дескриптора на каждый материал (diffuse, normal, displacement)
//  последние 3 слота - G-buffer для lighting pass
const UINT kSrvHeapSize = 1024;
const UINT kGBufferSrvSlot = kSrvHeapSize - GBuffer::kNumRenderTargets;
const UINT kShadowMapSrvSlot = kGBufferSrvSlot - 1; // слот перед G-буфером - каскадная карта теней
const int kWhiteTexSlot = 0;
const int kFlatNormalTexSlot = 1;
const int kBlackTexSlot = 2;
const int kFirstMaterialSlot = 3;

class App
{
public:
    void Init(HWND hwnd);
    void Update();
    void Draw();
    void Destroy();

private:
    // базовая инициализация DX12
    void CreateDeviceAndQueue();
    void CreateSwapChain(HWND hwnd);
    void CreateDescriptorHeaps();
    void CreateRenderTargets();
    void CreateDepthStencil();
    void CreateCommandObjects();
    void CreateFence();

    // загрузка модели и текстур
    void LoadModelAndTextures();
    void SetupMaterialAnimation(); // тут вручную выставляем тайлинг и скорость анимации
    void SetupSceneLights();       // расставляем источники света по сцене
    void UploadBufferData(const void* data, UINT64 size, ComPtr<ID3D12Resource>& outDefaultBuffer);
    bool CreateTextureFromWicFile(const std::wstring& filename, int srvSlot);
    void CreateDefaultTextures();
    ID3D12Resource* CreateTextureResourceAndUpload(UINT width, UINT height, const BYTE* pixels, UINT srcRowPitch, int srvSlot);
    void CreateSrv(ID3D12Resource* texture, int srvSlot);
    void LoadMaterialTexture(const std::string& fileName, int srvSlot, ID3D12Resource* fallback);

    // управление камерой и переключатели с клавиатуры
    void UpdateInput(float dt);

    // сцена с кучей объектов и отсечение
    void CreateMassScene();                         // раскидывает кубы и строит по ним октодерево
    void UpdateCulling(const XMMATRIX& viewProj);   // собирает список видимых кубов на этот кадр
    void UpdateWindowTitle(float dt);               // выводит в заголовок окна статистику и FPS

    // тени
    void UpdateShadows(const XMMATRIX& view);       // пересчитывает каскады и список объектов для карты теней

    // помощники
    void FlushCommandQueue();

private:
    HWND mHwnd = nullptr;

    // основные объекты D3D12
    ComPtr<ID3D12Device> mDevice;
    ComPtr<ID3D12CommandQueue> mCommandQueue;
    ComPtr<IDXGISwapChain3> mSwapChain;
    ComPtr<ID3D12CommandAllocator> mCommandAllocator;
    ComPtr<ID3D12GraphicsCommandList> mCommandList;

    // кучи дескрипторов
    ComPtr<ID3D12DescriptorHeap> mRtvHeap;
    ComPtr<ID3D12DescriptorHeap> mDsvHeap;
    ComPtr<ID3D12DescriptorHeap> mSrvHeap; // тут лежат SRV всех текстур и G-буфера
    UINT mRtvDescSize = 0;
    UINT mDsvDescSize = 0;
    UINT mSrvDescSize = 0;

    // бэк-буферы и depth buffer
    ComPtr<ID3D12Resource> mRenderTargets[kFrameCount];
    ComPtr<ID3D12Resource> mDepthStencilBuffer;
    UINT mCurrentBackBuffer = 0;

    // синхронизация CPU и GPU (максимально просто - флуш каждый кадр)
    ComPtr<ID3D12Fence> mFence;
    UINT64 mFenceValue = 0;
    HANDLE mFenceEvent = nullptr;

    // пайплайн (deferred rendering)
    RenderingSystem mRenderingSystem;

    // источники света сцены
    std::vector<Light> mLights;

    // геометрия модели
    ComPtr<ID3D12Resource> mVertexBuffer;
    ComPtr<ID3D12Resource> mIndexBuffer;
    D3D12_VERTEX_BUFFER_VIEW mVbv = {};
    D3D12_INDEX_BUFFER_VIEW mIbv = {};

    // загруженные данные модели и материалов
    LoadedModel mModel;

    // константные буферы (в Upload куче, замапленные на постоянку)
    ComPtr<ID3D12Resource> mObjectCB;
    BYTE* mObjectCBData = nullptr;
    UINT mObjectCBElementSize = 0;

    ComPtr<ID3D12Resource> mPassCB;
    BYTE* mPassCBData = nullptr;

    // вспомогательные ресурсы для загрузки (живут, пока GPU не скопирует данные)
    std::vector<ComPtr<ID3D12Resource>> mTextureUploadHeaps;
    std::vector<ComPtr<ID3D12Resource>> mTextures; // сами текстуры (default heap)

    // текстуры по умолчанию, их SRV подставляются, если у материала нет своей карты
    ID3D12Resource* mWhiteTexture = nullptr;
    ID3D12Resource* mFlatNormalTexture = nullptr;
    ID3D12Resource* mBlackTexture = nullptr;

    // таймер
    LARGE_INTEGER mFreq = {};
    LARGE_INTEGER mStartTime = {};
    LARGE_INTEGER mLastTime = {};

    UINT mClientWidth = kClientWidth;
    UINT mClientHeight = kClientHeight;

    // камера: позиция и углы поворота (yaw - влево-вправо, pitch - вверх-вниз)
    XMFLOAT3 mCameraPos = { 0.0f, 150.0f, -500.0f };
    float mCameraYaw = 0.0f;   // 0 = смотрим вдоль +Z
    float mCameraPitch = 0.0f;
    XMFLOAT3 mCameraForward = { 0.0f, 0.0f, 1.0f };

    // Параметры тесселяции и смещения. Числа подобраны под масштаб Sponza,
    // для модели другого размера их нужно поменять
    float mTessMinDist = 100.0f;      // ближе - максимальная детализация
    float mTessMaxDist = 1500.0f;     // дальше - треугольники не делятся
    float mTessMinFactor = 1.0f;
    float mTessMaxFactor = 16.0f;
    float mDisplacementScale = 2.5f;
    float mDisplacementBias = 0.0f;

    // переключатели с клавиатуры
    bool mTessellationEnabled = true; // T
    bool mWireframe = false;          // F
    bool mPrevKeyT = false;           // состояние клавиш в прошлом кадре, чтобы ловить именно нажатие
    bool mPrevKeyF = false;


 

    ComPtr<ID3D12Resource> mMassObjectCB;  // константы всех кубов, заполняются один раз, кубы не двигаются
    std::vector<AABB> mMassBoxes;          // ограничивающий объём каждого куба в мировых координатах
    std::vector<Light> mMassLights;        // в сцене с кубами только солнце

    // отсечение
    Frustum mFrustum;
    Octree mOctree;
    std::vector<uint32_t> mVisibleObjects; // номера кубов, которые рисуем в этом кадре
    CullingStats mCullingStats;
    float mCullingTimeMs = 0.0f;           // сколько CPU потратил на отсечение в этом кадре

    // переключатели
    bool mMassScene = false;      // M - Sponza со светом или сцена с кубами
    bool mCullingEnabled = true;  // C - frustum culling вкл/выкл
    bool mUseOctree = true;       // O - отсечение через октодерево или полным перебором
    bool mVSync = true;           // V - вертикальная синхронизация, без неё FPS не упирается в частоту монитора
    bool mPrevKeyM = false;
    bool mPrevKeyC = false;
    bool mPrevKeyO = false;
    bool mPrevKeyV = false;

    // счётчик FPS для заголовка окна
    UINT mFpsFrames = 0;
    float mFpsTimer = 0.0f;

    // тени
    std::vector<uint32_t> mShadowCasters; // копии, которые попали хотя бы в один каскад и рисуются в карту теней
    bool mPcfEnabled = true;              // P - мягкий край тени (PCF) или жёсткий
    bool mShowCascades = false;           // K - раскрасить пиксели по номеру каскада
    bool mPrevKeyP = false;
    bool mPrevKeyK = false;
};


void App::Init(HWND hwnd)
{
    QueryPerformanceFrequency(&mFreq);
    QueryPerformanceCounter(&mStartTime);
    mLastTime = mStartTime;
    mHwnd = hwnd;
    // WIC (загрузка картинок) требует COM
    ThrowIfFailed(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED), "CoInitializeEx");

    CreateDeviceAndQueue();
    CreateCommandObjects();
    CreateFence();
    CreateSwapChain(hwnd);
    CreateDescriptorHeaps();
    CreateRenderTargets();
    CreateDepthStencil();
    LoadModelAndTextures();
    CreateMassScene();

    mRenderingSystem.Init(mDevice.Get(), mClientWidth, mClientHeight,
        mSrvHeap.Get(), mSrvDescSize, kGBufferSrvSlot,
        kShadowMapSrvSlot, kShadowMapSize,
        DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_D24_UNORM_S8_UINT);

    SetupSceneLights();

    // дожидаемся пока всё что мы накомандовали на загрузку реально выполнится на GPU
    ThrowIfFailed(mCommandList->Close(), "CommandList->Close (init)");
    ID3D12CommandList* lists[] = { mCommandList.Get() };
    mCommandQueue->ExecuteCommandLists(1, lists);
    FlushCommandQueue();
}

void App::CreateDeviceAndQueue()
{
#if defined(_DEBUG)
    // включаем debug слой, чтобы D3D12 ругался в Output если мы что-то делаем не так
    ComPtr<ID3D12Debug> debugController;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
        debugController->EnableDebugLayer();
#endif

    ComPtr<IDXGIFactory4> factory;
    ThrowIfFailed(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");

    // пробуем создать устройство на реальной видеокарте
    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&mDevice));
    if (FAILED(hr))
    {
        // если не получилось (например нет нормальной видеокарты) - берём программный WARP адаптер
        ComPtr<IDXGIAdapter> warpAdapter;
        ThrowIfFailed(factory->EnumWarpAdapter(IID_PPV_ARGS(&warpAdapter)), "EnumWarpAdapter");
        ThrowIfFailed(D3D12CreateDevice(warpAdapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&mDevice)), "D3D12CreateDevice (warp)");
    }

    D3D12_COMMAND_QUEUE_DESC qDesc = {};
    qDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ThrowIfFailed(mDevice->CreateCommandQueue(&qDesc, IID_PPV_ARGS(&mCommandQueue)), "CreateCommandQueue");
}

void App::CreateCommandObjects()
{
    ThrowIfFailed(mDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&mCommandAllocator)), "CreateCommandAllocator");
    ThrowIfFailed(mDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, mCommandAllocator.Get(), nullptr, IID_PPV_ARGS(&mCommandList)), "CreateCommandList");
    // командный лист создаётся уже открытым, это нам и нужно для загрузки ресурсов в Init()
}

void App::CreateFence()
{
    ThrowIfFailed(mDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&mFence)), "CreateFence");
    mFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
}

void App::CreateSwapChain(HWND hwnd)
{
    ComPtr<IDXGIFactory4> factory;
    ThrowIfFailed(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1 (swapchain)");

    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = mClientWidth;
    desc.Height = mClientHeight;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = kFrameCount;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    ComPtr<IDXGISwapChain1> swapChain1;
    ThrowIfFailed(factory->CreateSwapChainForHwnd(mCommandQueue.Get(), hwnd, &desc, nullptr, nullptr, &swapChain1), "CreateSwapChainForHwnd");
    ThrowIfFailed(swapChain1.As(&mSwapChain), "SwapChain As IDXGISwapChain3");

    mCurrentBackBuffer = mSwapChain->GetCurrentBackBufferIndex();
}

void App::CreateDescriptorHeaps()
{
    // RTV куча - под бэк-буферы (kFrameCount штук)
    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
    rtvDesc.NumDescriptors = kFrameCount;
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    ThrowIfFailed(mDevice->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&mRtvHeap)), "CreateDescriptorHeap RTV");
    mRtvDescSize = mDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // DSV куча - под depth buffer (1 штука)
    D3D12_DESCRIPTOR_HEAP_DESC dsvDesc = {};
    dsvDesc.NumDescriptors = 1;
    dsvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    ThrowIfFailed(mDevice->CreateDescriptorHeap(&dsvDesc, IID_PPV_ARGS(&mDsvHeap)), "CreateDescriptorHeap DSV");
    mDsvDescSize = mDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

    // SRV куча - под текстуры материалов (по 3 на материал) и G-buffer, раскладка описана у kSrvHeapSize
    D3D12_DESCRIPTOR_HEAP_DESC srvDesc = {};
    srvDesc.NumDescriptors = kSrvHeapSize;
    srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE; // эта куча видна шейдерам
    ThrowIfFailed(mDevice->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&mSrvHeap)), "CreateDescriptorHeap SRV");
    mSrvDescSize = mDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void App::CreateRenderTargets()
{
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = mRtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < kFrameCount; ++i)
    {
        ThrowIfFailed(mSwapChain->GetBuffer(i, IID_PPV_ARGS(&mRenderTargets[i])), "SwapChain->GetBuffer");
        mDevice->CreateRenderTargetView(mRenderTargets[i].Get(), nullptr, rtvHandle);
        rtvHandle.ptr += mRtvDescSize;
    }
}

void App::CreateDepthStencil()
{
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = mClientWidth;
    desc.Height = mClientHeight;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    clearValue.DepthStencil.Depth = 1.0f;
    clearValue.DepthStencil.Stencil = 0;

    ThrowIfFailed(mDevice->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, &clearValue,
        IID_PPV_ARGS(&mDepthStencilBuffer)), "CreateCommittedResource DepthStencil");

    mDevice->CreateDepthStencilView(mDepthStencilBuffer.Get(), nullptr, mDsvHeap->GetCPUDescriptorHandleForHeapStart());
}

// 
// КОРНЕВАЯ СИГНАТУРА (Root Signature)
//
// У нас 3 штуки:
//  - b0: константы объекта (мир, тайлинг текстуры, цвет материала) - корневой дескриптор
//  - b1: константы кадра (камера) - корневой дескриптор
//  - t0: таблица с одной SRV (текстура) - таблица дескрипторов
// Плюс статический сэмплер s0, чтобы не делать отдельную кучу под сэмплеры

//void App::CreateRootSignature()
//{
//    D3D12_DESCRIPTOR_RANGE srvRange = {};
//    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
//    srvRange.NumDescriptors = 1;
//    srvRange.BaseShaderRegister = 0; // t0
//
//    D3D12_ROOT_PARAMETER params[3] = {};
//
//    // b0 - per object constants
//    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
//    params[0].Descriptor.ShaderRegister = 0;
//    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
//
//    // b1 - per pass constants
//    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
//    params[1].Descriptor.ShaderRegister = 1;
//    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
//
//    // t0 - текстура (через таблицу дескрипторов)
//    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
//    params[2].DescriptorTable.NumDescriptorRanges = 1;
//    params[2].DescriptorTable.pDescriptorRanges = &srvRange;
//    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
//
//    // статический сэмплер - линейная фильтрация + wrap (для тайлинга это то что нужно)
//    D3D12_STATIC_SAMPLER_DESC sampler = {};
//    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
//    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
//    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
//    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
//    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
//    sampler.MaxLOD = D3D12_FLOAT32_MAX;
//    sampler.ShaderRegister = 0; // s0
//    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
//
//    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
//    rsDesc.NumParameters = (UINT)(sizeof(params) / sizeof(params[0]));
//    rsDesc.pParameters = params;
//    rsDesc.NumStaticSamplers = 1;
//    rsDesc.pStaticSamplers = &sampler;
//    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
//
//    ComPtr<ID3DBlob> serialized, errorBlob;
//    HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errorBlob);
//    if (FAILED(hr))
//    {
//        if (errorBlob)
//            OutputDebugStringA((char*)errorBlob->GetBufferPointer());
//        ThrowIfFailed(hr, "D3D12SerializeRootSignature");
//    }
//
//    ThrowIfFailed(mDevice->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&mRootSignature)), "CreateRootSignature");
//}

 
// PIPELINE STATE (компилируем шейдеры и собираем PSO)
//void App::CreatePipelineState()
//{
//    ComPtr<ID3DBlob> vsBlob, psBlob, errorBlob;
//
//    UINT compileFlags = 0;
//#if defined(_DEBUG)
//    compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
//#endif
//
//    HRESULT hr = D3DCompileFromFile(L"Shaders.hlsl", nullptr, nullptr, "VS", "vs_5_0", compileFlags, 0, &vsBlob, &errorBlob);
//    if (FAILED(hr))
//    {
//        if (errorBlob) OutputDebugStringA((char*)errorBlob->GetBufferPointer());
//        ThrowIfFailed(hr, "Compile VS (проверь что Shaders.hlsl лежит рядом с exe)");
//    }
//
//    hr = D3DCompileFromFile(L"Shaders.hlsl", nullptr, nullptr, "PS", "ps_5_0", compileFlags, 0, &psBlob, &errorBlob);
//    if (FAILED(hr))
//    {
//        if (errorBlob) OutputDebugStringA((char*)errorBlob->GetBufferPointer());
//        ThrowIfFailed(hr, "Compile PS");
//    }
//
//    // описываем формат вершины - должно совпадать со структурой Vertex
//    D3D12_INPUT_ELEMENT_DESC inputLayout[] =
//    {
//        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
//        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
//        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
//    };
//
//    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
//    psoDesc.pRootSignature = mRootSignature.Get();
//    psoDesc.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
//    psoDesc.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
//    psoDesc.InputLayout = { inputLayout, (UINT)(sizeof(inputLayout) / sizeof(inputLayout[0])) };
//
//    // растеризация - обычная, рисуем закрашенные треугольники
//    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
//    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
//    psoDesc.RasterizerState.FrontCounterClockwise = FALSE;
//    psoDesc.RasterizerState.DepthClipEnable = TRUE;
//
//    // блендинг - выключен, текстуры у нас без прозрачности
//    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
//
//    // depth test - включен, как обычно для 3D
//    psoDesc.DepthStencilState.DepthEnable = TRUE;
//    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
//    psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
//    psoDesc.DepthStencilState.StencilEnable = FALSE;
//
//    psoDesc.SampleMask = UINT_MAX;
//    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
//    psoDesc.NumRenderTargets = 1;
//    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
//    psoDesc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
//    psoDesc.SampleDesc.Count = 1;
//
//    ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mPSO)), "CreateGraphicsPipelineState");
//}


// Загрузка данных в GPU буфер (вершины/индексы).
// Делаем через промежуточный Upload буфер, как рассказывали на лекции про текстуры -
// сначала кладём данные в Upload кучу, потом командой CopyResource переносим в Default кучу.

void App::UploadBufferData(const void* data, UINT64 size, ComPtr<ID3D12Resource>& outDefaultBuffer)
{
    D3D12_HEAP_PROPERTIES defaultHeapProps = {};
    defaultHeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_HEAP_PROPERTIES uploadHeapProps = {};
    uploadHeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC bufferDesc = {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = size;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    // целевой буфер - в быстрой видеопамяти (Default), сразу ставим как COPY_DEST
    ThrowIfFailed(mDevice->CreateCommittedResource(
        &defaultHeapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&outDefaultBuffer)), "CreateCommittedResource (default buffer)");

    // промежуточный буфер - в Upload куче, сюда CPU может писать напрямую
    ComPtr<ID3D12Resource> uploadBuffer;
    ThrowIfFailed(mDevice->CreateCommittedResource(
        &uploadHeapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadBuffer)), "CreateCommittedResource (upload buffer)");

    // копируем наши данные в upload буфер
    void* mapped = nullptr;
    ThrowIfFailed(uploadBuffer->Map(0, nullptr, &mapped), "Map upload buffer");
    memcpy(mapped, data, size);
    uploadBuffer->Unmap(0, nullptr);

    // командуем GPU скопировать из upload в default буфер
    mCommandList->CopyResource(outDefaultBuffer.Get(), uploadBuffer.Get());

    // переводим default буфер в состояние, в котором его можно использовать для рисования
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = outDefaultBuffer.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_GENERIC_READ;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    mCommandList->ResourceBarrier(1, &barrier);

    // upload буфер должен дожить до того момента когда GPU реально выполнит копирование,
    // поэтому держим его в общем списке и не отпускаем пока не сделаем Flush в конце Init()
    mTextureUploadHeaps.push_back(uploadBuffer);
}
// ЗАГРУЗКА ТЕКСТУРЫ ИЗ ФАЙЛА С ПОМОЩЬЮ WIC (Windows Imaging Component)
//
// WIC - стандартный способ читать PNG/JPG/BMP в Windows без сторонних библиотек.
// Достаём пиксели в формате RGBA8 и заливаем их в текстуру D3D12, как
// показывали на слайдах с CreateTextureFromScratch.

// Создаёт текстуру RGBA8 в default куче, заливает в неё пиксели и делает SRV в слот srvSlot.
// Возвращает указатель на текстуру (владеет ей вектор mTextures)
ID3D12Resource* App::CreateTextureResourceAndUpload(UINT width, UINT height, const BYTE* pixels, UINT srcRowPitch, int srvSlot)
{
    D3D12_HEAP_PROPERTIES defaultHeapProps = {};
    defaultHeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1; // мипмапы не делаем, одного уровня хватает
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    ComPtr<ID3D12Resource> texture;
    ThrowIfFailed(mDevice->CreateCommittedResource(
        &defaultHeapProps, D3D12_HEAP_FLAG_NONE, &texDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture)), "CreateCommittedResource (texture)");

    // узнаём у девайса, сколько байт нужно для промежуточного (upload) буфера
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT numRows = 0;
    UINT64 rowSizeInBytes = 0;
    UINT64 totalBytes = 0;
    mDevice->GetCopyableFootprints(&texDesc, 0, 1, 0, &footprint, &numRows, &rowSizeInBytes, &totalBytes);

    D3D12_HEAP_PROPERTIES uploadHeapProps = {};
    uploadHeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC uploadDesc = {};
    uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    uploadDesc.Width = totalBytes;
    uploadDesc.Height = 1;
    uploadDesc.DepthOrArraySize = 1;
    uploadDesc.MipLevels = 1;
    uploadDesc.SampleDesc.Count = 1;
    uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> uploadBuffer;
    ThrowIfFailed(mDevice->CreateCommittedResource(
        &uploadHeapProps, D3D12_HEAP_FLAG_NONE, &uploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadBuffer)), "CreateCommittedResource (texture upload)");

    // копируем картинку построчно, у исходных пикселей и у GPU буфера может быть разный rowPitch
    BYTE* mapped = nullptr;
    ThrowIfFailed(uploadBuffer->Map(0, nullptr, (void**)&mapped), "Map texture upload buffer");
    for (UINT y = 0; y < height; ++y)
    {
        memcpy(mapped + footprint.Footprint.RowPitch * y, pixels + srcRowPitch * y, min((UINT)rowSizeInBytes, srcRowPitch));
    }
    uploadBuffer->Unmap(0, nullptr);

    // командуем GPU скопировать из upload буфера в текстуру
    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = texture.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = uploadBuffer.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = footprint;

    mCommandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    // переводим текстуру в состояние "можно читать из шейдера"
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = texture.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    mCommandList->ResourceBarrier(1, &barrier);

    // создаём SRV для этой текстуры в нашей куче дескрипторов
    CreateSrv(texture.Get(), srvSlot);

    // держим upload буфер и саму текстуру живыми, пока GPU не выполнит копирование
    mTextureUploadHeaps.push_back(uploadBuffer);
    mTextures.push_back(texture);
    return texture.Get();
}

// Создаёт SRV для уже существующей текстуры в указанном слоте SRV кучи.
// Так одну и ту же текстуру по умолчанию можно подставить в слоты многих материалов
void App::CreateSrv(ID3D12Resource* texture, int srvSlot)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = texture->GetDesc().Format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    D3D12_CPU_DESCRIPTOR_HANDLE handle = mSrvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += (SIZE_T)srvSlot * mSrvDescSize;
    mDevice->CreateShaderResourceView(texture, &srvDesc, handle);
}
// Грузит файл картинки через WIC и создаёт из неё текстуру в указанный слот SRV кучи.
// Возвращает false, если файл не открылся, тогда слот нужно заполнить текстурой по умолчанию
bool App::CreateTextureFromWicFile(const std::wstring& filename, int srvSlot)
{
    ComPtr<IWICImagingFactory> wicFactory;
    ThrowIfFailed(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wicFactory)), "CoCreateInstance WICImagingFactory");

    ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = wicFactory->CreateDecoderFromFilename(filename.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder);
    if (FAILED(hr))
    {
        // файл не нашёлся или формат не поддерживается, вызывающий код подставит текстуру по умолчанию
        OutputDebugStringW((L"Не удалось открыть текстуру: " + filename + L" - будет текстура по умолчанию\n").c_str());
        return false;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    ThrowIfFailed(decoder->GetFrame(0, &frame), "WIC GetFrame");

    // переводим картинку в формат RGBA8, именно его мы и зальём в текстуру
    ComPtr<IWICFormatConverter> converter;
    ThrowIfFailed(wicFactory->CreateFormatConverter(&converter), "CreateFormatConverter");
    ThrowIfFailed(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom), "WIC Converter Initialize");

    UINT width = 0, height = 0;
    converter->GetSize(&width, &height);

    UINT rowPitch = width * 4; // 4 байта на пиксель (RGBA8)
    std::vector<BYTE> pixels(rowPitch * height);
    ThrowIfFailed(converter->CopyPixels(nullptr, rowPitch, (UINT)pixels.size(), pixels.data()), "WIC CopyPixels");

    CreateTextureResourceAndUpload(width, height, pixels.data(), rowPitch, srvSlot);
    return true;
}

// Создаёт три текстуры 1x1 для материалов, у которых нет своих карт:
//  белая - diffuse, итоговый цвет будет просто DiffuseAlbedo материала
//  (128,128,255) - плоская нормаль, после распаковки это (0,0,1), то есть нормаль вершины без изменений
//  чёрная - displacement, высота 0, вершины никуда не сдвигаются
// Они лежат в слотах 0, 1, 2 подряд, поэтому таблица, начатая со слота 0, сама по себе
// является корректным "материалом по умолчанию"
void App::CreateDefaultTextures()
{
    BYTE whitePixel[4] = { 255, 255, 255, 255 };
    BYTE flatNormalPixel[4] = { 128, 128, 255, 255 };
    BYTE blackPixel[4] = { 0, 0, 0, 255 };

    mWhiteTexture = CreateTextureResourceAndUpload(1, 1, whitePixel, 4, kWhiteTexSlot);
    mFlatNormalTexture = CreateTextureResourceAndUpload(1, 1, flatNormalPixel, 4, kFlatNormalTexSlot);
    mBlackTexture = CreateTextureResourceAndUpload(1, 1, blackPixel, 4, kBlackTexSlot);
}

// Грузит одну текстуру материала в слот srvSlot.
// Если имя файла пустое или файл не открылся, в слот кладётся SRV текстуры fallback
void App::LoadMaterialTexture(const std::string& fileName, int srvSlot, ID3D12Resource* fallback)
{
    if (!fileName.empty())
    {
        std::string fullPath = "obj\\" + fileName;
        OutputDebugStringA(("Гружу: " + fullPath + "\n").c_str());
        std::wstring wPath(fullPath.begin(), fullPath.end());

        if (CreateTextureFromWicFile(wPath, srvSlot))
            return;
    }
    CreateSrv(fallback, srvSlot);
}
// Тут мы вручную настраиваем тайлинг и анимацию для текстур по их названию материала.
// Это и есть та часть домашки, где "Добавьте текстурную анимацию и тайлинг".
// Если хочешь поменять под свою модель - просто измени тут значения

void App::SetupMaterialAnimation()
{
    for (auto& m : mModel.Materials)
    {
        // тайлинг - сколько раз текстура повторяется по U и V
        m.TileScale = XMFLOAT2(2.0f, 2.0f);

        // скорость анимации (сдвиг текстурных координат в секунду)
        // если поставить 0,0 - текстура будет статичная, без анимации
        m.ScrollSpeed = XMFLOAT2(0.1f, 0.05f);
    }
}
 
// Расставляем источники света по сцене: один Directional (солнце с ambient),
// и несколько Point/Spot, разбросанных вокруг модели (для Sponza - по всему холлу)
 
void App::SetupSceneLights()
{
    mLights.clear();

    // ---- Directional - "солнце", единственный источник, дающий ambient (слайд 13) ----
    Light sun;
    sun.Type = (int)LightType::Directional;
    sun.Direction = XMFLOAT3(0.5f, -1.0f, 0.3f);
    sun.Color = XMFLOAT3(1.0f, 0.95f, 0.85f);
    sun.Intensity = 1.0f;
    mLights.push_back(sun);

    // ---- Point light 1 ----
    Light p1;
    p1.Type = (int)LightType::Point;
    p1.Position = XMFLOAT3(-300.0f, 200.0f, 0.0f);
    p1.Range = 600.0f;
    p1.Color = XMFLOAT3(1.0f, 0.3f, 0.2f);
    p1.Intensity = 3.0f;
    mLights.push_back(p1);

    // ---- Point light 2 ----
    Light p2;
    p2.Type = (int)LightType::Point;
    p2.Position = XMFLOAT3(300.0f, 200.0f, 0.0f);
    p2.Range = 600.0f;
    p2.Color = XMFLOAT3(0.2f, 0.4f, 1.0f);
    p2.Intensity = 3.0f;
    mLights.push_back(p2);

    // ---- Point light 3  ----
    Light p3;
    p3.Type = (int)LightType::Point;
    p3.Position = XMFLOAT3(0.0f, 300.0f, 800.0f);
    p3.Range = 800.0f;
    p3.Color = XMFLOAT3(0.3f, 1.0f, 0.3f);
    p3.Intensity = 2.5f;
    mLights.push_back(p3);

    // ---- Spot light - "прожектор" сверху, направленный вниз на центр сцены ----
    Light spot;
    spot.Type = (int)LightType::Spot;
    spot.Position = XMFLOAT3(0.0f, 600.0f, 0.0f);
    spot.Direction = XMFLOAT3(0.0f, -1.0f, 0.0f);
    spot.Range = 1000.0f;
    spot.SpotPower = 8.0f;
    spot.Color = XMFLOAT3(1.0f, 1.0f, 1.0f);
    spot.Intensity = 4.0f;
    mLights.push_back(spot);
}

// Загружаем модель и все её текстуры

void App::LoadModelAndTextures()
{
    // ------ грузим геометрию и материалы из obj/model.obj ------
    mModel = LoadObjModel("obj", "model.obj");

    if (mModel.Vertices.empty())
        throw std::runtime_error("Модель пустая - проверь файл obj/model.obj");
    // базис tangent space для карт нормалей считаем до заливки вершин на GPU
    ComputeTangents(mModel);
    // общая нормаль сдвига и вес для displacement, чтобы грани не разъезжались
    ComputeDisplacementData(mModel);
    // настраиваем тайлинг/анимацию текстур
    SetupMaterialAnimation();
    
    // ------ вершинный и индексный буфер ------
    UploadBufferData(mModel.Vertices.data(), mModel.Vertices.size() * sizeof(Vertex), mVertexBuffer);
    UploadBufferData(mModel.Indices.data(), mModel.Indices.size() * sizeof(uint32_t), mIndexBuffer);

    mVbv.BufferLocation = mVertexBuffer->GetGPUVirtualAddress();
    mVbv.StrideInBytes = sizeof(Vertex);
    mVbv.SizeInBytes = (UINT)(mModel.Vertices.size() * sizeof(Vertex));

    mIbv.BufferLocation = mIndexBuffer->GetGPUVirtualAddress();
    mIbv.Format = DXGI_FORMAT_R32_UINT;
    mIbv.SizeInBytes = (UINT)(mModel.Indices.size() * sizeof(uint32_t));

    // ------ текстуры ------
    // слот 0 в SRV куче - всегда дефолтная белая текстура (для материалов без картинки)
        // Текстуры.
    // Слоты 0..2 - текстуры по умолчанию, дальше у каждого материала своя тройка
    // diffuse, normal, displacement. Шейдер получает одну таблицу из трёх дескрипторов,
    // начало которой и есть SrvIndex материала
    CreateDefaultTextures();
    int nextSrvSlot = kFirstMaterialSlot;

    for (auto& mat : mModel.Materials)
    {
        if (nextSrvSlot + 3 > (int)kShadowMapSrvSlot)
            throw std::runtime_error("Материалов слишком много, увеличь kSrvHeapSize");

        mat.SrvIndex = nextSrvSlot;
        LoadMaterialTexture(mat.DiffuseMapFile, nextSrvSlot + 0, mWhiteTexture);
        LoadMaterialTexture(mat.NormalMapFile, nextSrvSlot + 1, mFlatNormalTexture);
        LoadMaterialTexture(mat.DisplacementMapFile, nextSrvSlot + 2, mBlackTexture);
        nextSrvSlot += 3;
    }
 

    // если в модели вообще нет материалов (например, .obj без mtllib) -
    // на всякий случай добьём список одним материалом по умолчанию
    if (mModel.Materials.empty())
    {
        MaterialData def;
        def.SrvIndex = 0;
        def.TileScale = XMFLOAT2(1, 1);
        mModel.Materials.push_back(def);
        for (auto& sm : mModel.Submeshes)
            if (sm.MaterialIndex < 0) sm.MaterialIndex = 0;
    }

    // ------ константные буферы ------
    // под per-object данные - один блок на каждый сабмеш
    mObjectCBElementSize = CalcCBSize(sizeof(ObjectConstants));
    UINT objectCBTotalSize = mObjectCBElementSize * (UINT)mModel.Submeshes.size();

    D3D12_HEAP_PROPERTIES uploadHeapProps = {};
    uploadHeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC cbDesc = {};
    cbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    cbDesc.Width = objectCBTotalSize;
    cbDesc.Height = 1;
    cbDesc.DepthOrArraySize = 1;
    cbDesc.MipLevels = 1;
    cbDesc.SampleDesc.Count = 1;
    cbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ThrowIfFailed(mDevice->CreateCommittedResource(&uploadHeapProps, D3D12_HEAP_FLAG_NONE, &cbDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&mObjectCB)), "CreateCommittedResource ObjectCB");
    ThrowIfFailed(mObjectCB->Map(0, nullptr, (void**)&mObjectCBData), "Map ObjectCB");

    // под per-pass данные (камера) - один блок на 256 байт
    D3D12_RESOURCE_DESC passDesc = cbDesc;
    passDesc.Width = CalcCBSize(sizeof(PassConstants));
    ThrowIfFailed(mDevice->CreateCommittedResource(&uploadHeapProps, D3D12_HEAP_FLAG_NONE, &passDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&mPassCB)), "CreateCommittedResource PassCB");
    ThrowIfFailed(mPassCB->Map(0, nullptr, (void**)&mPassCBData), "Map PassCB");
}

// Сцена с кучей объектов: уменьшенные копии Sponza без текстур и без цвета,
// разного размера и поворота, разбросанные случайно.
// Копии неподвижные, поэтому их константы и октодерево строятся один раз при запуске.
// Геометрия своя не нужна - рисуем тот же вершинный и индексный буфер, что и основная Sponza
void App::CreateMassScene()
{
    // коробка вокруг Sponza в её собственных координатах, из неё для каждой копии получится мировой AABB
    AABB localBox = ComputeModelAABB(mModel);

    // константный буфер на все копии, по блоку на копию (каждый блок выровнен по 256 байт)
    D3D12_HEAP_PROPERTIES uploadHeapProps = {};
    uploadHeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC cbDesc = {};
    cbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    cbDesc.Width = (UINT64)mObjectCBElementSize * kMassObjectCount;
    cbDesc.Height = 1;
    cbDesc.DepthOrArraySize = 1;
    cbDesc.MipLevels = 1;
    cbDesc.SampleDesc.Count = 1;
    cbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ThrowIfFailed(mDevice->CreateCommittedResource(&uploadHeapProps, D3D12_HEAP_FLAG_NONE, &cbDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&mMassObjectCB)), "CreateCommittedResource MassObjectCB");

    BYTE* cbData = nullptr;
    ThrowIfFailed(mMassObjectCB->Map(0, nullptr, (void**)&cbData), "Map MassObjectCB");

    // фиксированное зерно - при каждом запуске копии стоят на тех же местах, удобно сравнивать замеры
    std::mt19937 rng(12345);
    std::uniform_real_distribution<float> distXZ(-kMassSceneHalfSize, kMassSceneHalfSize);
    std::uniform_real_distribution<float> distY(0.0f, kMassSceneHeight);
    std::uniform_real_distribution<float> distScale(0.05f, 0.15f); // полная Sponza ~3700 единиц в длину, копии ~200-550
    std::uniform_real_distribution<float> distAngle(0.0f, XM_2PI);

    mMassBoxes.resize(kMassObjectCount);

    for (UINT i = 0; i < kMassObjectCount; ++i)
    {
        // масштаб одинаковый по всем осям, поворот только вокруг вертикали - здания стоят ровно
        float scale = distScale(rng);
        XMMATRIX world =
            XMMatrixScaling(scale, scale, scale) *
            XMMatrixRotationY(distAngle(rng)) *
            XMMatrixTranslation(distXZ(rng), distY(rng), distXZ(rng));

        // мировой AABB копии
        mMassBoxes[i] = TransformAABB(localBox, world);

        // без текстур и без цвета: светло-серый альбедо, UV не трансформируются
        ObjectConstants objCB = {};
        XMStoreFloat4x4(&objCB.World, XMMatrixTranspose(world));
        XMStoreFloat4x4(&objCB.TexTransform, XMMatrixIdentity());
        XMStoreFloat4x4(&objCB.ReliefTexTransform, XMMatrixIdentity());
        objCB.DiffuseAlbedo = XMFLOAT4(0.8f, 0.8f, 0.8f, 1.0f);

        memcpy(cbData + (size_t)i * mObjectCBElementSize, &objCB, sizeof(objCB));
    }

    // данные больше не меняются, отображение можно снять, GPU читает upload-кучу напрямую
    mMassObjectCB->Unmap(0, nullptr);

    // октодерево строится один раз, т.к. объекты неподвижны
    mOctree.Build(mMassBoxes);

    char msg[128];
    sprintf_s(msg, "Октодерево построено: %zu узлов на %u объектов\n", mOctree.GetNodeCount(), kMassObjectCount);
    OutputDebugStringA(msg);

    // свет для сцены с копиями - одно солнце, оно же даёт ambient
    Light sun;
    sun.Type = (int)LightType::Directional;
    sun.Direction = XMFLOAT3(0.4f, -1.0f, 0.6f);
    sun.Color = XMFLOAT3(1.0f, 1.0f, 1.0f);
    sun.Intensity = 1.2f;
    mMassLights.push_back(sun);
}

// Отсечение кубов на этот кадр. Три режима:
//  выкл             - рисуем все кубы
//  перебор          - проверяем AABB каждого куба против фрустума
//  октодерево       - проверяем кубы узлов дерева, невидимые ветки отбрасываются целиком
void App::UpdateCulling(const XMMATRIX& viewProj)
{
    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);

    mVisibleObjects.clear();
    mCullingStats = {};

    if (!mCullingEnabled)
    {
        for (uint32_t i = 0; i < kMassObjectCount; ++i)
            mVisibleObjects.push_back(i);
    }
    else
    {
        mFrustum.FromViewProj(viewProj);

        if (mUseOctree)
        {
            mOctree.Query(mFrustum, mVisibleObjects, mCullingStats);
        }
        else
        {
            for (uint32_t i = 0; i < kMassObjectCount; ++i)
            {
                mCullingStats.AabbTests++;
                if (mFrustum.TestAABB(mMassBoxes[i]) != Frustum::Result::Outside)
                    mVisibleObjects.push_back(i);
            }
        }
    }

    QueryPerformanceCounter(&end);
    mCullingTimeMs = 1000.0f * (float)(end.QuadPart - start.QuadPart) / (float)mFreq.QuadPart;
}

// Тени на этот кадр:
//  находим направленный свет текущей сцены (солнце), тени отбрасывает только он
//  пересчитываем каскады под текущее положение камеры
//  в сцене с копиями выбираем объекты, которые попали хотя бы в один каскад
void App::UpdateShadows(const XMMATRIX& view)
{
    const std::vector<Light>& lights = mMassScene ? mMassLights : mLights;

    XMFLOAT3 sunDir(0.0f, -1.0f, 0.0f);
    for (const Light& l : lights)
    {
        if (l.Type == (int)LightType::Directional)
        {
            sunDir = l.Direction;
            break;
        }
    }

    CascadedShadowMap& shadowMap = mRenderingSystem.GetShadowMap();
    shadowMap.Update(view, kCameraFovY, (float)mClientWidth / (float)mClientHeight, kCameraNear,
        kShadowDistance, kCascadeLambda, sunDir, mPcfEnabled, mShowCascades);

    if (!mMassScene)
        return;

    // Отсечение для карты теней: каскад - это тоже камера (ортогональная), у неё тоже есть фрустум.
    // Объект рисуем в карту, только если он попал хотя бы в один из них.
    // Фрустум каскада уже растянут в сторону солнца, поэтому объекты, которые сами не видны камере,
    // но отбрасывают тень в кадр, тоже сюда попадают
    mShadowCasters.clear();

    if (!mCullingEnabled)
    {
        for (uint32_t i = 0; i < kMassObjectCount; ++i)
            mShadowCasters.push_back(i);
        return;
    }

    Frustum cascadeFrustums[CascadedShadowMap::kCascadeCount];
    for (UINT c = 0; c < CascadedShadowMap::kCascadeCount; ++c)
        cascadeFrustums[c].FromViewProj(shadowMap.GetCascadeViewProj(c));

    for (uint32_t i = 0; i < kMassObjectCount; ++i)
    {
        for (UINT c = 0; c < CascadedShadowMap::kCascadeCount; ++c)
        {
            if (cascadeFrustums[c].TestAABB(mMassBoxes[i]) != Frustum::Result::Outside)
            {
                mShadowCasters.push_back(i);
                break;
            }
        }
    }
}

// Два раза в секунду пишет в заголовок окна режим, число нарисованных объектов,
// число проверок AABB, время отсечения и FPS
void App::UpdateWindowTitle(float dt)
{
    mFpsFrames++;
    mFpsTimer += dt;
    if (mFpsTimer < 0.5f)
        return;

    float fps = (float)mFpsFrames / mFpsTimer;
    mFpsFrames = 0;
    mFpsTimer = 0.0f;

    wchar_t title[512];
    if (!mMassScene)
    {
        swprintf_s(title, L"Sponza | тесселяция %ls | тени: PCF %ls | FPS %.0f | V-Sync %ls | M - сцена с копиями",
            mTessellationEnabled ? L"вкл" : L"выкл", mPcfEnabled ? L"вкл" : L"выкл", fps, mVSync ? L"вкл" : L"выкл");
    }
    else
    {
        const wchar_t* mode = !mCullingEnabled ? L"выкл" : (mUseOctree ? L"фрустум + октодерево" : L"фрустум, перебор");
        swprintf_s(title, L"Копии Sponza | отсечение: %ls | рисуется %zu из %u | в тени %zu | проверок AABB %u | отсечение %.3f мс | PCF %ls | FPS %.0f | V-Sync %ls",
            mode, mVisibleObjects.size(), kMassObjectCount, mShadowCasters.size(), mCullingStats.AabbTests, mCullingTimeMs,
            mPcfEnabled ? L"вкл" : L"выкл", fps, mVSync ? L"вкл" : L"выкл");
    }
    SetWindowTextW(mHwnd, title);
}

// Управление:
//  WASD - движение, Q/E - вниз/вверх, Shift - быстрее
//  стрелки - поворот камеры
//  T - включить/выключить тесселяцию, F - каркасный режим
//  P - PCF вкл/выкл: мягкий край тени или ступеньки
//  K - 	раскрасить каскады: красный, зелёный, синий, жёлтый
void App::UpdateInput(float dt)
{
    // клавиши читаем только когда окно активно, иначе камера будет ездить при наборе текста в другом окне
    if (GetForegroundWindow() != mHwnd)
        return;

    auto IsDown = [](int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; };

    // поворот камеры
    const float turnSpeed = 1.5f * dt; // радиан за кадр
    if (IsDown(VK_LEFT))  mCameraYaw -= turnSpeed;
    if (IsDown(VK_RIGHT)) mCameraYaw += turnSpeed;
    if (IsDown(VK_UP))    mCameraPitch += turnSpeed;
    if (IsDown(VK_DOWN))  mCameraPitch -= turnSpeed;

    // не даём камере перевернуться через вертикаль
    const float maxPitch = XM_PIDIV2 - 0.05f;
    if (mCameraPitch > maxPitch)  mCameraPitch = maxPitch;
    if (mCameraPitch < -maxPitch) mCameraPitch = -maxPitch;

    // направление взгляда из углов (левая система координат, yaw = 0 смотрит вдоль +Z)
    XMVECTOR forward = XMVectorSet(
        sinf(mCameraYaw) * cosf(mCameraPitch),
        sinf(mCameraPitch),
        cosf(mCameraYaw) * cosf(mCameraPitch), 0.0f);
    XMVECTOR right = XMVectorSet(cosf(mCameraYaw), 0.0f, -sinf(mCameraYaw), 0.0f);
    XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    XMStoreFloat3(&mCameraForward, forward);

    // движение камеры
    float moveSpeed = 400.0f * dt;
    if (IsDown(VK_SHIFT)) moveSpeed *= 3.0f;

    XMVECTOR pos = XMLoadFloat3(&mCameraPos);
    if (IsDown('W')) pos = XMVectorAdd(pos, XMVectorScale(forward, moveSpeed));
    if (IsDown('S')) pos = XMVectorSubtract(pos, XMVectorScale(forward, moveSpeed));
    if (IsDown('D')) pos = XMVectorAdd(pos, XMVectorScale(right, moveSpeed));
    if (IsDown('A')) pos = XMVectorSubtract(pos, XMVectorScale(right, moveSpeed));
    if (IsDown('E')) pos = XMVectorAdd(pos, XMVectorScale(up, moveSpeed));
    if (IsDown('Q')) pos = XMVectorSubtract(pos, XMVectorScale(up, moveSpeed));
    XMStoreFloat3(&mCameraPos, pos);

    // переключатели срабатывают только в момент нажатия, а не пока клавиша зажата
    bool keyT = IsDown('T');
    if (keyT && !mPrevKeyT) mTessellationEnabled = !mTessellationEnabled;
    mPrevKeyT = keyT;

    bool keyF = IsDown('F');
    if (keyF && !mPrevKeyF) mWireframe = !mWireframe;
    mPrevKeyF = keyF;


    // переключение сцены: Sponza со светом <-> кубы
    bool keyM = IsDown('M');
    if (keyM && !mPrevKeyM) mMassScene = !mMassScene;
    mPrevKeyM = keyM;

    // frustum culling вкл/выкл
    bool keyC = IsDown('C');
    if (keyC && !mPrevKeyC) mCullingEnabled = !mCullingEnabled;
    mPrevKeyC = keyC;

    // отсечение через октодерево или полным перебором
    bool keyO = IsDown('O');
    if (keyO && !mPrevKeyO) mUseOctree = !mUseOctree;
    mPrevKeyO = keyO;

    // вертикальная синхронизация вкл/выкл
    bool keyV = IsDown('V');
    if (keyV && !mPrevKeyV) mVSync = !mVSync;
    mPrevKeyV = keyV;


    // PCF вкл/выкл: мягкий или ступенчатый край тени
    bool keyP = IsDown('P');
    if (keyP && !mPrevKeyP) mPcfEnabled = !mPcfEnabled;
    mPrevKeyP = keyP;

    // раскраска каскадов вкл/выкл
    bool keyK = IsDown('K');
    if (keyK && !mPrevKeyK) mShowCascades = !mShowCascades;
    mPrevKeyK = keyK;

}

// UPDATE - тут считаем камеру и обновляем константные буферы
void App::Update()
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    float totalTime = (float)(now.QuadPart - mStartTime.QuadPart) / (float)mFreq.QuadPart;

    // время кадра, ограничиваем сверху, чтобы после паузы (перетаскивание окна) камера не улетела
    float dt = (float)(now.QuadPart - mLastTime.QuadPart) / (float)mFreq.QuadPart;
    mLastTime = now;
    if (dt > 0.1f) dt = 0.1f;

    UpdateInput(dt);
    mRenderingSystem.SetWireframe(mWireframe);

    XMVECTOR eyePos = XMLoadFloat3(&mCameraPos);
    XMVECTOR forward = XMLoadFloat3(&mCameraForward);
    XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

    XMMATRIX view = XMMatrixLookToLH(eyePos, forward, up);
    XMMATRIX proj = XMMatrixPerspectiveFovLH(kCameraFovY, (float)mClientWidth / (float)mClientHeight, kCameraNear, kCameraFar);// дальность прорисовки

    PassConstants passCB = {};
    XMStoreFloat4x4(&passCB.ViewProj, XMMatrixTranspose(view * proj));
    passCB.EyePosW = mCameraPos;
    passCB.TessMinDist = mTessMinDist;
    passCB.TessMaxDist = mTessMaxDist;

    // у кубов нет карт высот, тесселяция там только тратила бы время
    if (mTessellationEnabled && !mMassScene)
    {
        passCB.TessMinFactor = mTessMinFactor;
        passCB.TessMaxFactor = mTessMaxFactor;
        passCB.DisplacementScale = mDisplacementScale;
        passCB.DisplacementBias = mDisplacementBias;
    }
    else
    {
        // коэффициент 1 - тесселятор отдаёт исходный треугольник как есть, смещения тоже нет
        passCB.TessMinFactor = 1.0f;
        passCB.TessMaxFactor = 1.0f;
        passCB.DisplacementScale = 0.0f;
        passCB.DisplacementBias = 0.0f;
    }
    memcpy(mPassCBData, &passCB, sizeof(passCB));

    // для каждого сабмеша обновляем мировую матрицу и матрицу текстурного трансформа
    for (size_t i = 0; i < mModel.Submeshes.size(); ++i)
    {
        const Submesh& sm = mModel.Submeshes[i];
        const MaterialData& mat = mModel.Materials[sm.MaterialIndex >= 0 ? sm.MaterialIndex : 0];

        ObjectConstants objCB;
        XMStoreFloat4x4(&objCB.World, XMMatrixIdentity()); // модель стоит в центре, можно тут крутить/двигать

        // тайлинг - просто масштаб UV координат
        XMMATRIX tileScale = XMMatrixScaling(mat.TileScale.x, mat.TileScale.y, 1.0f);

        // анимация - сдвигаем UV координаты по времени (текстура "ползёт")
        float offsetU = totalTime * mat.ScrollSpeed.x;
        float offsetV = totalTime * mat.ScrollSpeed.y;
        XMMATRIX scroll = XMMatrixTranslation(offsetU, offsetV, 0.0f);

        XMMATRIX texTransform = tileScale * scroll;
        XMStoreFloat4x4(&objCB.TexTransform, XMMatrixTranspose(texTransform));

        // рельеф берёт тот же тайлинг, но без сдвига, чтобы геометрия стояла на месте
        XMStoreFloat4x4(&objCB.ReliefTexTransform, XMMatrixTranspose(tileScale));
        objCB.DiffuseAlbedo = mat.DiffuseAlbedo;

        memcpy(mObjectCBData + i * mObjectCBElementSize, &objCB, sizeof(objCB));
    }


    // отсечение считается по той же матрице, что уходит в шейдер, только не транспонированной
    if (mMassScene)
        UpdateCulling(view * proj);

    // каскады зависят от камеры, поэтому пересчитываются каждый кадр
    UpdateShadows(view);

    UpdateWindowTitle(dt);
}


// DRAW - рисуем кадр: deferred rendering из двух проходов
//   1) Geometry pass - заполняем G-buffer (без освещения)
//   2) Lighting pass - для каждого источника света накапливаем освещение в бэк-буфере
void App::Draw()
{
    ThrowIfFailed(mCommandAllocator->Reset(), "CommandAllocator->Reset");
    // PSO передаём nullptr - RenderingSystem сам выставит нужный PSO в начале каждого прохода
    ThrowIfFailed(mCommandList->Reset(mCommandAllocator.Get(), nullptr), "CommandList->Reset");

    D3D12_VIEWPORT viewport = { 0.0f, 0.0f, (float)mClientWidth, (float)mClientHeight, 0.0f, 1.0f };
    D3D12_RECT scissor = { 0, 0, (LONG)mClientWidth, (LONG)mClientHeight };
    mCommandList->RSSetViewports(1, &viewport);
    mCommandList->RSSetScissorRects(1, &scissor);

    // подключаем кучу с текстурами и SRV G-буфера
    ID3D12DescriptorHeap* heaps[] = { mSrvHeap.Get() };
    mCommandList->SetDescriptorHeaps(1, heaps);

    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = mDsvHeap->GetCPUDescriptorHandleForHeapStart();


    // Shadow pass: сцена с точки зрения солнца во все каскады карты теней.
    // Текстуры не нужны, только позиции, поэтому модель рисуется целиком одним вызовом, без деления на сабмеши
    mRenderingSystem.BeginShadowPass(mCommandList.Get());

    mCommandList->IASetVertexBuffers(0, 1, &mVbv);
    mCommandList->IASetIndexBuffer(&mIbv);
    UINT modelIndexCount = (UINT)mModel.Indices.size();

    if (mMassScene)
    {
        // только копии, которые попали хотя бы в один каскад
        D3D12_GPU_VIRTUAL_ADDRESS cbStart = mMassObjectCB->GetGPUVirtualAddress();
        for (uint32_t id : mShadowCasters)
        {
            mCommandList->SetGraphicsRootConstantBufferView(0, cbStart + (UINT64)id * mObjectCBElementSize);
            mCommandList->DrawIndexedInstanced(modelIndexCount, 1, 0, 0, 0);
        }
    }
    else
    {
        // у всех сабмешей Sponza одна и та же мировая матрица, берём константы первого
        mCommandList->SetGraphicsRootConstantBufferView(0, mObjectCB->GetGPUVirtualAddress());
        mCommandList->DrawIndexedInstanced(modelIndexCount, 1, 0, 0, 0);
    }

    mRenderingSystem.EndShadowPass(mCommandList.Get());

    // shadow pass поменял viewport на размер карты теней, возвращаем размер окна
    mCommandList->RSSetViewports(1, &viewport);
    mCommandList->RSSetScissorRects(1, &scissor);

    // Geometry pass: заполняем G-buffer
    mRenderingSystem.BeginGeometryPass(mCommandList.Get(), dsvHandle);

    // b1 - константы кадра (камера и тесселяция), общие для всех объектов
    mCommandList->SetGraphicsRootConstantBufferView(1, mPassCB->GetGPUVirtualAddress());

    // топологию (патчи из 3 точек) уже выставил BeginGeometryPass, тут только буферы
    if (mMassScene)
    {
        // Сцена с копиями: геометрия основной Sponza, но без текстур, и своя мировая матрица у каждой копии.
        // Материал у всех копий один, поэтому вся модель рисуется одним вызовом, без деления на сабмеши.
        // Рисуем только копии из списка видимых, который собрал UpdateCulling
        mCommandList->IASetVertexBuffers(0, 1, &mVbv);
        mCommandList->IASetIndexBuffer(&mIbv);

        // без текстур: таблица со слота 0 - это белая, плоская нормаль и чёрная
        mCommandList->SetGraphicsRootDescriptorTable(2, mSrvHeap->GetGPUDescriptorHandleForHeapStart());

        UINT indexCount = (UINT)mModel.Indices.size();
        D3D12_GPU_VIRTUAL_ADDRESS cbStart = mMassObjectCB->GetGPUVirtualAddress();
        for (uint32_t id : mVisibleObjects)
        {
            mCommandList->SetGraphicsRootConstantBufferView(0, cbStart + (UINT64)id * mObjectCBElementSize);
            mCommandList->DrawIndexedInstanced(indexCount, 1, 0, 0, 0);
        }
    }
    else
    {
        mCommandList->IASetVertexBuffers(0, 1, &mVbv);
        mCommandList->IASetIndexBuffer(&mIbv);

        for (size_t i = 0; i < mModel.Submeshes.size(); ++i)
        {
            const Submesh& sm = mModel.Submeshes[i];
            if (sm.IndexCount == 0)
                continue;

            const MaterialData& mat = mModel.Materials[sm.MaterialIndex >= 0 ? sm.MaterialIndex : 0];

            D3D12_GPU_VIRTUAL_ADDRESS objCbAddress = mObjectCB->GetGPUVirtualAddress() + i * mObjectCBElementSize;
            mCommandList->SetGraphicsRootConstantBufferView(0, objCbAddress);

            // таблица из 3 текстур материала: diffuse, normal, displacement
            D3D12_GPU_DESCRIPTOR_HANDLE srvHandle = mSrvHeap->GetGPUDescriptorHandleForHeapStart();
            srvHandle.ptr += (SIZE_T)mat.SrvIndex * mSrvDescSize;
            mCommandList->SetGraphicsRootDescriptorTable(2, srvHandle);

            mCommandList->DrawIndexedInstanced(sm.IndexCount, 1, sm.StartIndexLocation, 0, 0);
        }
    }

    mRenderingSystem.EndGeometryPass(mCommandList.Get());

    // Lighting pass: переводим бэк-буфер в render target, очищаем и накапливаем освещение
    D3D12_RESOURCE_BARRIER toRT = {};
    toRT.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toRT.Transition.pResource = mRenderTargets[mCurrentBackBuffer].Get();
    toRT.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    toRT.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    toRT.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    mCommandList->ResourceBarrier(1, &toRT);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = mRtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvHandle.ptr += mCurrentBackBuffer * mRtvDescSize;

    // очищаем бэк-буфер в чёрный, lighting pass будет складывать яркость сверху
    const float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    mCommandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

    // камера и матрица ViewProj нужны lighting pass'у для расчёта блика и затухания
    XMFLOAT4X4 viewProj;
    memcpy(&viewProj, mPassCBData, sizeof(XMFLOAT4X4)); // ViewProj лежит первым полем в PassConstants

    // в сцене с кубами свой набор света (только солнце), в Sponza - как раньше
    mRenderingSystem.RenderLights(mCommandList.Get(), rtvHandle, mMassScene ? mMassLights : mLights, viewProj, mCameraPos);

    // переводим бэк-буфер обратно в состояние "готов к показу"
    D3D12_RESOURCE_BARRIER toPresent = {};
    toPresent.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toPresent.Transition.pResource = mRenderTargets[mCurrentBackBuffer].Get();
    toPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    toPresent.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    toPresent.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    mCommandList->ResourceBarrier(1, &toPresent);

    ThrowIfFailed(mCommandList->Close(), "CommandList->Close (draw)");

    ID3D12CommandList* lists[] = { mCommandList.Get() };
    mCommandQueue->ExecuteCommandLists(1, lists);

    // 1 - ждать вертикальную синхронизацию, 0 - показывать сразу (FPS не ограничен частотой монитора)
    ThrowIfFailed(mSwapChain->Present(mVSync ? 1 : 0, 0), "Present");

    FlushCommandQueue();

    mCurrentBackBuffer = mSwapChain->GetCurrentBackBufferIndex();
}

void App::FlushCommandQueue()
{
    mFenceValue++;
    ThrowIfFailed(mCommandQueue->Signal(mFence.Get(), mFenceValue), "CommandQueue->Signal");

    if (mFence->GetCompletedValue() < mFenceValue)
    {
        ThrowIfFailed(mFence->SetEventOnCompletion(mFenceValue, mFenceEvent), "Fence->SetEventOnCompletion");
        WaitForSingleObject(mFenceEvent, INFINITE);
    }
}

void App::Destroy()
{
    FlushCommandQueue();

    if (mObjectCB) mObjectCB->Unmap(0, nullptr);
    if (mPassCB) mPassCB->Unmap(0, nullptr);

    if (mFenceEvent) CloseHandle(mFenceEvent);
    CoUninitialize();
}


// WIN32 - окно и главный цикл

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE)
            PostQuitMessage(0);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int)
{
    // создаём окно
    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"DX12HomeworkWindow";
    RegisterClassEx(&wc);

    RECT rect = { 0, 0, (LONG)kClientWidth, (LONG)kClientHeight };
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

    HWND hwnd = CreateWindow(
        wc.lpszClassName, L"DX12 - Текстуры, материалы, тайлинг и анимация",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top,
        nullptr, nullptr, hInstance, nullptr);

    ShowWindow(hwnd, SW_SHOW);

    App app;
    try
    {
        app.Init(hwnd);
    }
    catch (const std::exception& e)
    {
        MessageBoxA(hwnd, e.what(), "Ошибка инициализации", MB_OK | MB_ICONERROR);
        return -1;
    }

    // основной цикл сообщений + рендер
    MSG msg = {};
    while (msg.message != WM_QUIT)
    {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        else
        {
            try
            {
                app.Update();
                app.Draw();
            }
            catch (const std::exception& e)
            {
                MessageBoxA(hwnd, e.what(), "Ошибка во время рендера", MB_OK | MB_ICONERROR);
                break;
            }
        }
    }

    app.Destroy();
    return 0;
}

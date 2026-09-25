// Culling.h
// Всё, что нужно для отсечения невидимых объектов:
//  AABB    - ограничивающий объём объекта, коробка со сторонами вдоль осей мира
//  Frustum - пирамида видимости камеры из 6 плоскостей, достаётся из матрицы ViewProj
//  Octree  - окто-дерево: пространство делится на 8 кубов, те ещё на 8 и так далее,
//            объекты раскладываются по этим кубам, и целые ветки дерева можно
//            отбросить одной проверкой, если их куб не попал в камеру

#pragma once

#include <DirectXMath.h>
#include <vector>
#include <cstdint>

using namespace DirectX;

// Axis-Aligned Bounding Box: минимальный и максимальный угол коробки в мировых координатах
struct AABB
{
    XMFLOAT3 Min = { 0, 0, 0 };
    XMFLOAT3 Max = { 0, 0, 0 };

    // коробка, которая целиком лежит внутри другой
    bool IsInside(const AABB& outer) const
    {
        return Min.x >= outer.Min.x && Min.y >= outer.Min.y && Min.z >= outer.Min.z &&
            Max.x <= outer.Max.x && Max.y <= outer.Max.y && Max.z <= outer.Max.z;
    }
};

// Переводит AABB модели из её локальных координат в мировые:
// берём 8 углов локальной коробки, переводим их мировой матрицей
// и по ним заново находим минимум и максимум по каждой оси.
// После поворота коробка уже не выровнена по осям, поэтому мировой AABB получается чуть больше
AABB TransformAABB(const AABB& local, const XMMATRIX& world);

// Пирамида видимости камеры
class Frustum
{
public:
    // Результат проверки объёма
    enum class Result
    {
        Outside,   // целиком снаружи - не рисуем
        Intersect, // частично внутри - рисуем, но детей в дереве надо проверять дальше
        Inside     // целиком внутри - рисуем, и всё, что внутри, можно больше не проверять
    };

    // достаёт 6 плоскостей из матрицы view * proj (не транспонированной)
    void FromViewProj(const XMMATRIX& viewProj);

    // проверка AABB против всех 6 плоскостей
    Result TestAABB(const AABB& box) const;

private:
    // плоскость хранится как (nx, ny, nz, d): точка p внутри, если dot(n, p) + d >= 0
    XMFLOAT4 mPlanes[6] = {};
};

// Статистика одного прохода отсечения, чтобы сравнивать способы между собой
struct CullingStats
{
    uint32_t AabbTests = 0; // сколько раз вызывали проверку AABB против фрустума
    uint32_t NodesVisited = 0; // сколько узлов дерева посетили (для перебора всегда 0)
};

// Окто-дерево над ограничивающими объёмами объектов сцены
class Octree
{
public:
    // Строит дерево по списку AABB объектов, индекс в векторе = номер объекта.
    // maxDepth - сколько раз максимум можно делить куб,
    // maxObjectsPerNode - если в узле объектов не больше, узел дальше не делим
    void Build(const std::vector<AABB>& objectBoxes, int maxDepth = 8, int maxObjectsPerNode = 8);

    // Собирает в outVisible номера объектов, которые хоть частично попали во фрустум
    void Query(const Frustum& frustum, std::vector<uint32_t>& outVisible, CullingStats& stats) const;

    size_t GetNodeCount() const { return mNodes.size(); }

private:
    struct Node
    {
        AABB Bounds;                   // куб, который покрывает этот узел
        int Children[8];               // индексы детей в mNodes, -1 если ребёнка нет
        std::vector<uint32_t> Objects; // объекты, которые не поместились целиком ни в одного ребёнка
    };

    // рекурсивно делит узел, раздавая объекты детям
    void BuildNode(int nodeIndex, std::vector<uint32_t>& objects, int depth);

    // рекурсивный обход при запросе; fullyInside = родитель уже целиком во фрустуме
    void QueryNode(int nodeIndex, const Frustum& frustum, bool fullyInside,
        std::vector<uint32_t>& outVisible, CullingStats& stats) const;

    // AABB одного из 8 детей: bit 0 - половина по X, bit 1 - по Y, bit 2 - по Z
    static AABB ChildBounds(const AABB& parent, int childIndex);

    std::vector<Node> mNodes;         // все узлы дерева, mNodes[0] - корень
    std::vector<AABB> mObjectBoxes;   // копия AABB объектов, нужна при запросе
    int mMaxDepth = 8;
    int mMaxObjectsPerNode = 8;
};
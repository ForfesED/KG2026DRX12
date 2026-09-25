// Culling.cpp

#include "Culling.h"
#include <cfloat>
#include <cmath>

AABB TransformAABB(const AABB& local, const XMMATRIX& world)
{
    AABB box;
    box.Min = XMFLOAT3(FLT_MAX, FLT_MAX, FLT_MAX);
    box.Max = XMFLOAT3(-FLT_MAX, -FLT_MAX, -FLT_MAX);

    // перебираем 8 углов: каждый бит номера выбирает минимум или максимум по своей оси
    for (int i = 0; i < 8; ++i)
    {
        XMVECTOR corner = XMVectorSet(
            (i & 1) ? local.Max.x : local.Min.x,
            (i & 2) ? local.Max.y : local.Min.y,
            (i & 4) ? local.Max.z : local.Min.z, 1.0f);

        XMFLOAT3 p;
        XMStoreFloat3(&p, XMVector3TransformCoord(corner, world));

        box.Min.x = fminf(box.Min.x, p.x);
        box.Min.y = fminf(box.Min.y, p.y);
        box.Min.z = fminf(box.Min.z, p.z);
        box.Max.x = fmaxf(box.Max.x, p.x);
        box.Max.y = fmaxf(box.Max.y, p.y);
        box.Max.z = fmaxf(box.Max.z, p.z);
    }
    return box;
}

// Точка в мире p переходит в clip space как (x, y, z, w) = p * ViewProj.
// Точка видна, если -w <= x <= w, -w <= y <= w, 0 <= z <= w (в D3D глубина от 0 до w).
// Каждое из этих неравенств - это плоскость. Например, x >= -w значит p * (col4 + col1) >= 0,
// где colN - это N-й столбец матрицы. Так и получаются 6 плоскостей.
void Frustum::FromViewProj(const XMMATRIX& viewProj)
{
    XMFLOAT4X4 m;
    XMStoreFloat4x4(&m, viewProj);

    // столбцы матрицы
    XMFLOAT4 col1 = { m._11, m._21, m._31, m._41 };
    XMFLOAT4 col2 = { m._12, m._22, m._32, m._42 };
    XMFLOAT4 col3 = { m._13, m._23, m._33, m._43 };
    XMFLOAT4 col4 = { m._14, m._24, m._34, m._44 };

    auto Add = [](const XMFLOAT4& a, const XMFLOAT4& b) { return XMFLOAT4(a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w); };
    auto Sub = [](const XMFLOAT4& a, const XMFLOAT4& b) { return XMFLOAT4(a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w); };

    mPlanes[0] = Add(col4, col1); // левая
    mPlanes[1] = Sub(col4, col1); // правая
    mPlanes[2] = Add(col4, col2); // нижняя
    mPlanes[3] = Sub(col4, col2); // верхняя
    mPlanes[4] = col3;            // ближняя
    mPlanes[5] = Sub(col4, col3); // дальняя

    // нормализуем, чтобы dot(n, p) + d был настоящим расстоянием до плоскости
    for (XMFLOAT4& plane : mPlanes)
    {
        float len = sqrtf(plane.x * plane.x + plane.y * plane.y + plane.z * plane.z);
        if (len > 0.0f)
        {
            plane.x /= len;
            plane.y /= len;
            plane.z /= len;
            plane.w /= len;
        }
    }
}

// Для каждой плоскости берём два угла коробки:
//  positive vertex - угол, дальше всех продвинутый вдоль нормали плоскости
//  negative vertex - угол, дальше всех в обратную сторону
// Если даже positive vertex снаружи, то вся коробка снаружи этой плоскости, значит и снаружи фрустума.
// Если negative vertex снаружи, а positive внутри - коробка пересекает плоскость.
Frustum::Result Frustum::TestAABB(const AABB& box) const
{
    bool intersect = false;

    for (const XMFLOAT4& plane : mPlanes)
    {
        XMFLOAT3 pv(
            plane.x >= 0.0f ? box.Max.x : box.Min.x,
            plane.y >= 0.0f ? box.Max.y : box.Min.y,
            plane.z >= 0.0f ? box.Max.z : box.Min.z);

        XMFLOAT3 nv(
            plane.x >= 0.0f ? box.Min.x : box.Max.x,
            plane.y >= 0.0f ? box.Min.y : box.Max.y,
            plane.z >= 0.0f ? box.Min.z : box.Max.z);

        if (plane.x * pv.x + plane.y * pv.y + plane.z * pv.z + plane.w < 0.0f)
            return Result::Outside;

        if (plane.x * nv.x + plane.y * nv.y + plane.z * nv.z + plane.w < 0.0f)
            intersect = true;
    }

    return intersect ? Result::Intersect : Result::Inside;
}

AABB Octree::ChildBounds(const AABB& parent, int childIndex)
{
    XMFLOAT3 center(
        0.5f * (parent.Min.x + parent.Max.x),
        0.5f * (parent.Min.y + parent.Max.y),
        0.5f * (parent.Min.z + parent.Max.z));

    // каждый бит номера ребёнка выбирает нижнюю или верхнюю половину по своей оси
    AABB child;
    child.Min.x = (childIndex & 1) ? center.x : parent.Min.x;
    child.Max.x = (childIndex & 1) ? parent.Max.x : center.x;
    child.Min.y = (childIndex & 2) ? center.y : parent.Min.y;
    child.Max.y = (childIndex & 2) ? parent.Max.y : center.y;
    child.Min.z = (childIndex & 4) ? center.z : parent.Min.z;
    child.Max.z = (childIndex & 4) ? parent.Max.z : center.z;
    return child;
}

void Octree::Build(const std::vector<AABB>& objectBoxes, int maxDepth, int maxObjectsPerNode)
{
    mObjectBoxes = objectBoxes;
    mMaxDepth = maxDepth;
    mMaxObjectsPerNode = maxObjectsPerNode;
    mNodes.clear();

    if (mObjectBoxes.empty())
        return;

    // корень - общий AABB всех объектов, растянутый до куба,
    // чтобы при делении пополам дети тоже были кубами, а не вытянутыми коробками
    AABB root = mObjectBoxes[0];
    for (const AABB& b : mObjectBoxes)
    {
        root.Min.x = fminf(root.Min.x, b.Min.x);
        root.Min.y = fminf(root.Min.y, b.Min.y);
        root.Min.z = fminf(root.Min.z, b.Min.z);
        root.Max.x = fmaxf(root.Max.x, b.Max.x);
        root.Max.y = fmaxf(root.Max.y, b.Max.y);
        root.Max.z = fmaxf(root.Max.z, b.Max.z);
    }

    float sizeX = root.Max.x - root.Min.x;
    float sizeY = root.Max.y - root.Min.y;
    float sizeZ = root.Max.z - root.Min.z;
    float halfSize = 0.5f * fmaxf(sizeX, fmaxf(sizeY, sizeZ));
    XMFLOAT3 center(
        0.5f * (root.Min.x + root.Max.x),
        0.5f * (root.Min.y + root.Max.y),
        0.5f * (root.Min.z + root.Max.z));

    Node rootNode;
    rootNode.Bounds.Min = XMFLOAT3(center.x - halfSize, center.y - halfSize, center.z - halfSize);
    rootNode.Bounds.Max = XMFLOAT3(center.x + halfSize, center.y + halfSize, center.z + halfSize);
    for (int& c : rootNode.Children) c = -1;
    mNodes.push_back(rootNode);

    // сначала все объекты лежат в корне, дальше BuildNode раздаёт их вниз
    std::vector<uint32_t> all(mObjectBoxes.size());
    for (uint32_t i = 0; i < (uint32_t)all.size(); ++i)
        all[i] = i;

    BuildNode(0, all, 0);
}

void Octree::BuildNode(int nodeIndex, std::vector<uint32_t>& objects, int depth)
{
    // мало объектов или дерево уже глубокое - это лист, все объекты остаются здесь
    if ((int)objects.size() <= mMaxObjectsPerNode || depth >= mMaxDepth)
    {
        mNodes[nodeIndex].Objects = std::move(objects);
        return;
    }

    // Раскладываем объекты: если объект целиком помещается в куб ребёнка, он уходит туда.
    // Объект, лежащий на границе между детьми, остаётся в этом узле.
    AABB childBounds[8];
    std::vector<uint32_t> childObjects[8];
    std::vector<uint32_t> stayHere;

    for (int c = 0; c < 8; ++c)
        childBounds[c] = ChildBounds(mNodes[nodeIndex].Bounds, c);

    for (uint32_t id : objects)
    {
        bool placed = false;
        for (int c = 0; c < 8; ++c)
        {
            if (mObjectBoxes[id].IsInside(childBounds[c]))
            {
                childObjects[c].push_back(id);
                placed = true;
                break;
            }
        }
        if (!placed)
            stayHere.push_back(id);
    }

    mNodes[nodeIndex].Objects = std::move(stayHere);

    // пустых детей не создаём, в них нечего искать
    for (int c = 0; c < 8; ++c)
    {
        if (childObjects[c].empty())
            continue;

        Node child;
        child.Bounds = childBounds[c];
        for (int& cc : child.Children) cc = -1;

        // push_back может перевыделить память вектора, поэтому ссылку на родителя не держим, только индекс
        int childIndex = (int)mNodes.size();
        mNodes.push_back(child);
        mNodes[nodeIndex].Children[c] = childIndex;

        BuildNode(childIndex, childObjects[c], depth + 1);
    }
}

void Octree::Query(const Frustum& frustum, std::vector<uint32_t>& outVisible, CullingStats& stats) const
{
    if (mNodes.empty())
        return;
    QueryNode(0, frustum, false, outVisible, stats);
}

void Octree::QueryNode(int nodeIndex, const Frustum& frustum, bool fullyInside,
    std::vector<uint32_t>& outVisible, CullingStats& stats) const
{
    const Node& node = mNodes[nodeIndex];
    stats.NodesVisited++;

    // Если родитель уже целиком во фрустуме, то и этот узел целиком внутри, проверка не нужна.
    // Иначе проверяем куб узла: если он снаружи, отбрасываем всю ветку со всеми объектами разом.
    if (!fullyInside)
    {
        stats.AabbTests++;
        Frustum::Result r = frustum.TestAABB(node.Bounds);
        if (r == Frustum::Result::Outside)
            return;
        if (r == Frustum::Result::Inside)
            fullyInside = true;
    }

    // объекты этого узла: если узел целиком внутри, берём всех без проверки,
    // если узел на границе фрустума, проверяем каждый объект отдельно
    for (uint32_t id : node.Objects)
    {
        if (fullyInside)
        {
            outVisible.push_back(id);
        }
        else
        {
            stats.AabbTests++;
            if (frustum.TestAABB(mObjectBoxes[id]) != Frustum::Result::Outside)
                outVisible.push_back(id);
        }
    }

    for (int c = 0; c < 8; ++c)
    {
        if (node.Children[c] >= 0)
            QueryNode(node.Children[c], frustum, fullyInside, outVisible, stats);
    }
}
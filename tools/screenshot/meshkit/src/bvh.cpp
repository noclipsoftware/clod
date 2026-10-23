// Binned SAH BVH build: 16 bins per axis, split where the surface area heuristic is cheapest.

#include "bvh.h"

static const int BINS = 16;

static float SahCost(const aabb& L, int CountL, const aabb& R, int CountR)
{
    return SurfaceArea(L) * CountL + SurfaceArea(R) * CountR;
}

int BuildNode(bvh& B, int First, int Count, int Depth)
{
    aabb Bounds = BoundsOf(B, First, Count);
    if(Count <= 2 || Depth > 32) { return MakeLeaf(B, Bounds, First, Count); }

    split Best = FindBestSplit(B, First, Count, Bounds, BINS, SahCost);
    if(Best.Cost >= SurfaceArea(Bounds) * Count) { return MakeLeaf(B, Bounds, First, Count); }

    int Mid = Partition(B, First, Count, Best);
    int Node = MakeInner(B, Bounds);
    B.Nodes[Node].Left = BuildNode(B, First, Mid - First, Depth + 1);
    B.Nodes[Node].Right = BuildNode(B, Mid, First + Count - Mid, Depth + 1);
    return Node;
}

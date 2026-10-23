// Mesh simplification by edge collapse with quadric error metrics (Garland & Heckbert 97).
// Each vertex keeps the sum of the plane quadrics of its faces; collapsing an edge moves the
// pair to the point that minimizes their combined quadric, cheapest edge first.

#include "simplify.h"
#include "quadric.h"
#include "heap.h"

static quadric FaceQuadric(const mesh& M, int F)
{
    vec3 A = M.P[M.Tri[F].V[0]], B = M.P[M.Tri[F].V[1]], C = M.P[M.Tri[F].V[2]];
    vec3 N = Normalize(Cross(B - A, C - A));
    return PlaneQuadric(N, -Dot(N, A));
}

static collapse BestCollapse(const mesh& M, const quadric* Q, int V0, int V1)
{
    quadric Sum = Q[V0] + Q[V1];
    collapse C = {V0, V1};
    if(!SolveMinimum(Sum, &C.Target))
    {
        C.Target = (M.P[V0] + M.P[V1]) * 0.5f; // singular: fall back to the midpoint
    }
    C.Cost = Evaluate(Sum, C.Target);
    return C;
}

int Simplify(mesh& M, int TargetFaces)
{
    std::vector<quadric> Q(M.P.size());
    for(int F = 0; F < (int)M.Tri.size(); ++F)
    {
        quadric FQ = FaceQuadric(M, F);
        for(int K = 0; K < 3; ++K) { Q[M.Tri[F].V[K]] += FQ; }
    }

    min_heap<collapse> Heap;
    for(const edge& E : UniqueEdges(M)) { Heap.Push(BestCollapse(M, Q.data(), E.V0, E.V1)); }

    int Faces = (int)M.Tri.size();
    while(Faces > TargetFaces && !Heap.Empty())
    {
        collapse C = Heap.Pop();
        if(M.Removed[C.V0] || M.Removed[C.V1] || C.Stamp != M.Stamp[C.V0]) { continue; }
        if(FlipsAnyFace(M, C)) { continue; } // would fold the surface over itself

        M.P[C.V0] = C.Target;
        Q[C.V0] += Q[C.V1];
        Faces -= MergeVertex(M, C.V1, C.V0);
        ++M.Stamp[C.V0];
        for(int N : Neighbors(M, C.V0)) { Heap.Push(BestCollapse(M, Q.data(), C.V0, N)); }
    }
    return Faces;
}

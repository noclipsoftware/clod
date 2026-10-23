// Half-edge connectivity for an indexed mesh. Boundary half-edges get Twin = -1.

#include "halfedge.h"
#include <unordered_map>

halfedge_mesh BuildHalfEdges(const mesh& M)
{
    halfedge_mesh H = {};
    std::unordered_map<unsigned long long, int> Open;
    for(int F = 0; F < (int)M.Tri.size(); ++F)
    {
        for(int K = 0; K < 3; ++K)
        {
            int A = M.Tri[F].V[K], B = M.Tri[F].V[(K + 1) % 3];
            int E = (int)H.Edges.size();
            H.Edges.push_back({A, -1, F * 3 + (K + 1) % 3});
            auto Twin = Open.find(((unsigned long long)B << 32) | (unsigned)A);
            if(Twin != Open.end()) { H.Edges[E].Twin = Twin->second; H.Edges[Twin->second].Twin = E; Open.erase(Twin); }
            else { Open[((unsigned long long)A << 32) | (unsigned)B] = E; }
        }
    }
    return H;
}

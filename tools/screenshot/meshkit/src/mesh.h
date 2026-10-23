#pragma once
#include <vector>
#include "vec3.h"

struct tri { int V[3]; };

// Indexed triangle mesh. Removed and Stamp are per vertex, used while simplifying.
struct mesh
{
    std::vector<vec3> P;
    std::vector<tri> Tri;
    std::vector<bool> Removed;
    std::vector<int> Stamp;
};

bool LoadObj(mesh& Out, const char* Path);
bool LoadPly(mesh& Out, const char* Path);

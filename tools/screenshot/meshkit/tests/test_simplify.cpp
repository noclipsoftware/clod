// test_simplify <mesh.obj> <ratio>: simplifies to ratio of the faces and checks the result is manifold.

#include <stdio.h>
#include <stdlib.h>
#include "mesh.h"
#include "simplify.h"

int main(int Argc, char** Argv)
{
    mesh M = {};
    if(Argc < 3 || !LoadObj(M, Argv[1])) { printf("usage: test_simplify <mesh.obj> <ratio>\n"); return 1; }
    int Before = (int)M.Tri.size();
    int After = Simplify(M, (int)(Before * atof(Argv[2])));
    printf("%d -> %d faces, %s\n", Before, After, IsManifold(M) ? "manifold" : "NOT MANIFOLD");
    return IsManifold(M) ? 0 : 1;
}

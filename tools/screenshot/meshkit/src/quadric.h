#pragma once
#include "vec3.h"

// Symmetric 4x4 error quadric, stored as its 10 unique terms.
struct quadric { double A[10]; };

quadric PlaneQuadric(vec3 N, float D);
quadric operator+(const quadric& X, const quadric& Y);
quadric& operator+=(quadric& X, const quadric& Y);
double Evaluate(const quadric& Q, vec3 P);
bool SolveMinimum(const quadric& Q, vec3* Out);

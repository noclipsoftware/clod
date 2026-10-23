#pragma once
#include <vector>
#include <algorithm>

// Binary min-heap on T::Cost. Stale entries are skipped by the caller (vertex stamps), not removed.
template <typename T>
struct min_heap
{
    std::vector<T> Items;

    bool Empty() const { return Items.empty(); }
    void Push(const T& X)
    {
        Items.push_back(X);
        std::push_heap(Items.begin(), Items.end(), [](const T& A, const T& B) { return A.Cost > B.Cost; });
    }
    T Pop()
    {
        std::pop_heap(Items.begin(), Items.end(), [](const T& A, const T& B) { return A.Cost > B.Cost; });
        T X = Items.back();
        Items.pop_back();
        return X;
    }
};

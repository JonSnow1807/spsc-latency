#pragma once
#include "queue.hpp"
namespace spsc {
template<class T> using cached = detail::queue<T, detail::design::cached>;
}

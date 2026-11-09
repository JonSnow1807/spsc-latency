#pragma once
#include "queue.hpp"
namespace spsc {
template<class T> using naive = detail::queue<T, detail::design::naive>;
}

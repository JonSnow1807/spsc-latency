#pragma once
#include "queue.hpp"
namespace spsc {
template<class T> using padded = detail::queue<T, detail::design::padded>;
}

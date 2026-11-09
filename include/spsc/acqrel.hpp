#pragma once
#include "queue.hpp"
namespace spsc {
// Experimental control: naive's layout with padded's memory ordering.
template<class T> using acqrel = detail::queue<T, detail::design::acqrel>;
}

#pragma once
#include <cstddef>
#include <functional>

namespace absent {

size_t parallelWidth();
void parallelFor(size_t count, const std::function<void(size_t)>& job);

}

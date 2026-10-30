#include "Shutdown.h"

#include <atomic>

namespace Mosaic {
namespace Shutdown {

namespace {

std::atomic<bool> g_requested{false};

}  // namespace

void request() { g_requested.store(true, std::memory_order_relaxed); }

bool requested() { return g_requested.load(std::memory_order_relaxed); }

}  // namespace Shutdown
}  // namespace Mosaic

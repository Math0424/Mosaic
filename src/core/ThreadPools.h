#pragma once

class QThreadPool;

namespace Mosaic {

// Dedicated worker pools, kept apart from QThreadPool::globalInstance()
// (CPU work: imports, duplicate search, feed scoring). Network calls sleep
// for rate limiting and block on replies; on the global pool enough of them
// starve everything else, and a task that waits on other pool tasks
// (MultiSource) can end up waiting on work that never gets a thread.
namespace ThreadPools {

// Blocking fetches and searches: thumbnails, media bytes, source queries.
QThreadPool* network();

// MultiSource member searches. Separate from network() because the
// MultiSource search itself runs on network() and waits on these.
QThreadPool* fanOut();

// Waits for these pools and the global pool to drain, sharing one
// deadline. Returns false on timeout.
bool waitForDone(int msecs);

}  // namespace ThreadPools

}  // namespace Mosaic

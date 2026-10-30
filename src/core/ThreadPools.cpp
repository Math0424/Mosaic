#include "ThreadPools.h"

#include <QDeadlineTimer>
#include <QThreadPool>

namespace Mosaic {
namespace ThreadPools {

namespace {

constexpr int kNetworkThreads = 8;
constexpr int kFanOutThreads = 16;

QThreadPool* makePool(int threads) {
    auto* pool = new QThreadPool;  // process lifetime; drained on shutdown
    pool->setMaxThreadCount(threads);
    return pool;
}

}  // namespace

QThreadPool* network() {
    static QThreadPool* pool = makePool(kNetworkThreads);
    return pool;
}

QThreadPool* fanOut() {
    static QThreadPool* pool = makePool(kFanOutThreads);
    return pool;
}

bool waitForDone(int msecs) {
    const QDeadlineTimer deadline(msecs);
    // Outermost first: network tasks may still be waiting on fanOut tasks.
    for (QThreadPool* pool : {network(), fanOut(), QThreadPool::globalInstance()}) {
        if (!pool->waitForDone(deadline))
            return false;
    }
    return true;
}

}  // namespace ThreadPools
}  // namespace Mosaic

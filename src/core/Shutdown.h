#pragma once

namespace Mosaic {

// Process-wide cooperative cancellation. MainWindow::closeEvent requests it;
// long worker loops (library scan, tag refresh, imports, duplicate search)
// and blocking HTTP calls poll it and stop at the next safe point, so the
// thread pool drains instead of being killed mid-write.
namespace Shutdown {

void request();

bool requested();

}  // namespace Shutdown

}  // namespace Mosaic

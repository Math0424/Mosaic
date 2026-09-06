#pragma once

#include <QStringList>

namespace Mosaic {

// DuckDB catalog is single-writer, so a second copy must refuse to start.
namespace InstanceGuard {

// Full paths of other running copies (by exe name); empty when none. Windows only.
QStringList otherInstances();

}  // namespace InstanceGuard
}  // namespace Mosaic

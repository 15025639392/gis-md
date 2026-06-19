#pragma once

#include <string>

namespace earth_engine {

struct Diagnostics;
struct PresentationTrace;

namespace minimal_globe_demo {

std::string buildRenderEntryDiagnosticsLine(const Diagnostics& diagnostics);
std::string buildPresentationTraceSummary(const PresentationTrace& trace);

} // namespace minimal_globe_demo
} // namespace earth_engine

#pragma once

#include <string_view>

namespace otc {

// Generated into the DLL at build time from configs/calibrations/pe-*.json.
// The returned view remains valid for the lifetime of the module.
std::string_view embeddedCalibrationJson(std::string_view buildId);

} // namespace otc


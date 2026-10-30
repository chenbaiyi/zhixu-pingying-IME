#pragma once

#include <filesystem>
#include <span>

#include <tl/expected.hpp>

#include "common/diagnostics/sanitized_logger.h"

namespace zhixu::diagnostics {

enum class DiagnosticExportError { Io };

[[nodiscard]] tl::expected<void, DiagnosticExportError> ExportDiagnostics(
    const std::filesystem::path& output, std::span<const Event> events);

}  // namespace zhixu::diagnostics

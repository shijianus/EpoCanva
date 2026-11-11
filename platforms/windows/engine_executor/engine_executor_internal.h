// EpoCanva P3.5 -- internal construction seam.
//
// Production callers use EngineExecutor(ExecutorOptions), which builds the real backend.
// Scheduler tests use EngineExecutorTestFactory::Create to inject a controlled backend. The
// factory is not a product capability: it is the one friend of the private executor
// constructor and it is declared here, outside the public header, so ordinary callers cannot
// supply a backend or reach the raw Runtime/Session/Controller through one.
#ifndef EPOCANVA_WINDOWS_ENGINE_EXECUTOR_ENGINE_EXECUTOR_INTERNAL_H_
#define EPOCANVA_WINDOWS_ENGINE_EXECUTOR_ENGINE_EXECUTOR_INTERNAL_H_

#include <memory>

#include "engine_backend.h"
#include "engine_executor.h"

namespace epocanva {
namespace windows {
namespace engine {

std::unique_ptr<EngineBackend> MakeRealEngineBackend();
std::unique_ptr<EngineBackend> MakeUnifiedEngineBackend(UnifiedBackendConfig config);

class EngineExecutorTestFactory {
 public:
  static std::unique_ptr<EngineExecutor> Create(ExecutorOptions options,
                                                std::unique_ptr<EngineBackend> backend);
};

}  // namespace engine
}  // namespace windows
}  // namespace epocanva

#endif  // EPOCANVA_WINDOWS_ENGINE_EXECUTOR_ENGINE_EXECUTOR_INTERNAL_H_

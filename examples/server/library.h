#pragma once

#include "runtime.h"

namespace httplib {
class Server;
}

struct AsyncGenerationJob;

json make_job_summary(const AsyncGenerationJob& job, const json& body);
// Writes a completed image or video job to --output-dir with a JSON sidecar per file.
void save_job_outputs(const ServerRuntime& runtime, const AsyncGenerationJob& job);
void register_library_endpoints(httplib::Server& svr, ServerRuntime& runtime);

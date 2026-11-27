#pragma once

#include "runtime.h"

namespace httplib {
class Server;
}

struct AsyncGenerationJob;

json make_job_summary(const AsyncGenerationJob& job, const json& body);
// Writes a completed image or video job to --output-dir with a JSON sidecar per file.
// Keeps the uploaded images, audio and control frames of a request in --input-dir and returns
// their URLs by request field, for the result's metadata.
json save_job_inputs(const ServerRuntime& runtime, const json& body);
void save_job_outputs(const ServerRuntime& runtime, const AsyncGenerationJob& job);
void register_library_endpoints(httplib::Server& svr, ServerRuntime& runtime);

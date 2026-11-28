#pragma once

#include "runtime.h"

namespace httplib {
class Server;
}

// Reads the library's auto-fit plan from its log lines to learn where each model part runs.
void observe_sd_log(const char* line);
// Where the current model runs: {kind: gpu|cpu|mixed, label, components: [{name, compute, params}]}.
json current_placement();
// GET /sdcpp/v1/system: compute devices, whether this is a GPU build, and whether a GPU was found.
void register_device_endpoints(httplib::Server& svr, ServerRuntime& runtime);

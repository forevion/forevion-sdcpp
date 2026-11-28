#pragma once

#include "runtime.h"

namespace httplib {
class Server;
}

void register_model_manager_endpoints(httplib::Server& svr, ServerRuntime& runtime);

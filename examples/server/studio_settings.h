#pragma once

#include "runtime.h"

namespace httplib {
class Server;
}

// Reads --settings-file and uses its models, inputs and outputs folders when they exist.
void apply_studio_settings(SDSvrParams& params);
// Folder setup for the Web UI: read and save the three folders, and browse folders on this computer.
void register_studio_settings_endpoints(httplib::Server& svr, ServerRuntime& runtime);

#pragma once

#include "httplib.h"
#include "runtime.h"

// Model downloads for the Web UI: a catalog of packages (built-in downloads.json plus
// <model-dir>/downloads/*.json), fetched one at a time with curl, checked against their
// SHA-256 and optionally packed to GGUF before they appear in the model catalog.
void register_download_endpoints(httplib::Server& svr, ServerRuntime& runtime);
void stop_downloads();
// Returns an error message when a {"packages": [...]} catalog file is not valid, else "".
std::string check_download_catalog(const json& catalog);
// Re-reads the download catalog after its files changed on disk.
void reload_download_catalog();

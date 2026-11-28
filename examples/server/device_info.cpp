#include "httplib.h"
#include "device_info.h"

#include <filesystem>
#include <map>
#include <mutex>
#include <regex>

#include "ggml-backend.h"

namespace fs = std::filesystem;

namespace {

struct PlacementState {
    std::mutex mutex;
    std::map<std::string, std::string> device_descriptions;
    json components = json::array();
    bool reading    = false;
};

PlacementState& placement_state() {
    static PlacementState state;
    return state;
}

std::string trimmed(const char* text) {
    std::string value = text ? text : "";
    const auto begin  = value.find_first_not_of(" 	");
    const auto end    = value.find_last_not_of(" 	");
    return begin == std::string::npos ? "" : value.substr(begin, end - begin + 1);
}

const char* device_type_name(enum ggml_backend_dev_type type) {
    switch (type) {
        case GGML_BACKEND_DEVICE_TYPE_GPU:
            return "gpu";
        case GGML_BACKEND_DEVICE_TYPE_IGPU:
            return "igpu";
        case GGML_BACKEND_DEVICE_TYPE_ACCEL:
            return "accel";
        default:
            return "cpu";
    }
}

bool is_gpu_name(const std::string& name) {
    return !name.empty() && name != "CPU" && name != "RAM" && name != "disk" && name.rfind("CPU", 0) != 0;
}

// A GPU backend library (ggml-cuda, ggml-vulkan, ...) ships next to the server, even if it failed to load.
std::string gpu_backend_library() {
    std::error_code error;
    const fs::path dir = fs::current_path(error);
    std::vector<fs::path> places;
#ifdef _WIN32
    wchar_t buffer[MAX_PATH];
    if (GetModuleFileNameW(nullptr, buffer, MAX_PATH))
        places.push_back(fs::path(buffer).parent_path());
#else
    places.push_back(fs::read_symlink("/proc/self/exe", error).parent_path());
#endif
    places.push_back(dir);
    for (const auto& place : places) {
        for (fs::directory_iterator it(place, error), end; !error && it != end; it.increment(error)) {
            const std::string name = it->path().filename().u8string();
            for (const char* backend : {"ggml-cuda", "ggml-hip", "ggml-vulkan", "ggml-sycl", "ggml-metal", "ggml-musa"})
                if (name.find(backend) != std::string::npos)
                    return backend + 5;
        }
    }
    return "";
}

}  // namespace

void observe_sd_log(const char* line) {
    if (line == nullptr)
        return;
    const std::string text(line);
    if (text.find("auto-fit") == std::string::npos && text.find(" -> compute ") == std::string::npos &&
        text.find(" free ") == std::string::npos)
        return;
    static const std::regex device_line(R"(^\s+(\S+)\s+(.*?)\s+free\s+-?\d+ MiB, budget)");
    static const std::regex component_line(R"(^\s+(\S+)\s+params\s+(\d+) MiB, compute reserve\s+\d+ MiB -> compute (\S+), params (\S+))");
    auto& state = placement_state();
    std::lock_guard<std::mutex> lock(state.mutex);
    std::smatch match;
    if (text.find("auto-fit plan:") != std::string::npos) {
        state.reading = true;
        state.components = json::array();
    } else if (state.reading && std::regex_search(text, match, component_line)) {
        state.components.push_back({{"name", match[1].str()},
                                    {"params_mib", std::stoll(match[2].str())},
                                    {"compute", match[3].str()},
                                    {"params", match[4].str()}});
    } else if (state.reading && std::regex_search(text, match, device_line)) {
        state.device_descriptions[match[1].str()] = trimmed(match[2].str().c_str());
    } else if (text.find("auto-fit: --backend") != std::string::npos) {
        state.reading = false;
    }
}

json current_placement() {
    auto& state = placement_state();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.components.empty())
        return {{"kind", "cpu"}, {"label", "CPU"}, {"components", json::array()}};
    bool any_gpu = false, all_gpu = true, spilled = false;
    std::string gpu;
    for (const auto& component : state.components) {
        const std::string compute = component["compute"];
        const std::string params  = component["params"];
        if (is_gpu_name(compute)) {
            any_gpu = true;
            if (gpu.empty())
                gpu = state.device_descriptions.count(compute) ? state.device_descriptions[compute] : compute;
            spilled |= !is_gpu_name(params);
        } else {
            all_gpu = false;
        }
    }
    std::string kind  = !any_gpu ? "cpu" : all_gpu && !spilled ? "gpu" : "mixed";
    std::string label = !any_gpu ? "CPU" : "GPU · " + gpu;
    if (kind == "mixed")
        label += all_gpu ? " (some weights in RAM)" : " + CPU";
    return {{"kind", kind}, {"label", label}, {"components", state.components}};
}

void register_device_endpoints(httplib::Server& svr, ServerRuntime& runtime) {
    ServerRuntime* rt = &runtime;
    svr.Get("/sdcpp/v1/system", [rt](const httplib::Request&, httplib::Response& res) {
        sd_list_devices(nullptr, 0);  // loads the dynamic backends when nothing is registered yet
        json devices    = json::array();
        bool gpu_found  = false;
        for (size_t i = 0; i < ggml_backend_dev_count(); ++i) {
            ggml_backend_dev_t dev = ggml_backend_dev_get(i);
            size_t free = 0, total = 0;
            ggml_backend_dev_memory(dev, &free, &total);
            const char* type = device_type_name(ggml_backend_dev_type(dev));
            gpu_found |= std::string(type) == "gpu";
            devices.push_back({{"name", trimmed(ggml_backend_dev_name(dev))},
                               {"description", trimmed(ggml_backend_dev_description(dev))},
                               {"type", type},
                               {"memory_total", total},
                               {"memory_free", free}});
        }
        const std::string gpu_build = gpu_backend_library();
        json body                   = {{"devices", devices},
                                       {"gpu_build", gpu_build},
                                       {"gpu_found", gpu_found},
                                       {"auto_fit", rt->ctx_params->auto_fit.value_or(true)},
                                       {"placement", current_placement()}};
        res.set_content(body.dump(), "application/json");
    });
}

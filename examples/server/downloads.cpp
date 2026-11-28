#include "downloads.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <regex>
#include <sstream>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "common/log.h"
#include "downloads_builtin.h"
#include "model_catalog.h"

namespace fs = std::filesystem;

namespace {

class Sha256 {
    std::array<uint32_t, 8> h_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::array<uint8_t, 64> block_{};
    size_t block_size_ = 0;
    uint64_t length_   = 0;

    static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

    void transform(const uint8_t* data) {
        static const uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t(data[i * 4]) << 24) | (uint32_t(data[i * 4 + 1]) << 16) | (uint32_t(data[i * 4 + 2]) << 8) | data[i * 4 + 3];
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i]              = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            h = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
        }
        h_[0] += a, h_[1] += b, h_[2] += c, h_[3] += d, h_[4] += e, h_[5] += f, h_[6] += g, h_[7] += h;
    }

public:
    void update(const uint8_t* data, size_t size) {
        length_ += size;
        if (block_size_ > 0) {
            const size_t take = std::min(size, 64 - block_size_);
            std::memcpy(block_.data() + block_size_, data, take);
            block_size_ += take, data += take, size -= take;
            if (block_size_ < 64)
                return;
            transform(block_.data());
            block_size_ = 0;
        }
        for (; size >= 64; data += 64, size -= 64)
            transform(data);
        std::memcpy(block_.data(), data, size);
        block_size_ = size;
    }

    std::string hex() {
        const uint64_t bits = length_ * 8;
        const uint8_t pad   = 0x80;
        update(&pad, 1);
        const uint8_t zero = 0;
        while (block_size_ != 56)
            update(&zero, 1);
        for (int i = 7; i >= 0; --i) {
            const uint8_t byte = static_cast<uint8_t>(bits >> (i * 8));
            update(&byte, 1);
        }
        static const char* digits = "0123456789abcdef";
        std::string out;
        for (uint32_t word : h_)
            for (int i = 28; i >= 0; i -= 4)
                out.push_back(digits[(word >> i) & 0xF]);
        return out;
    }
};

double total_ram_gb() {
#ifdef _WIN32
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status))
        return static_cast<double>(status.ullTotalPhys) / (1024.0 * 1024.0 * 1024.0);
    return 0;
#else
    const long pages = sysconf(_SC_PHYS_PAGES), page = sysconf(_SC_PAGE_SIZE);
    return pages > 0 && page > 0 ? static_cast<double>(pages) * page / (1024.0 * 1024.0 * 1024.0) : 0;
#endif
}

uintmax_t file_size_or_zero(const fs::path& path) {
    std::error_code error;
    const auto size = fs::file_size(path, error);
    return error ? 0 : size;
}

// Runs curl with `args` (no shell). Callers validate URLs, so no argument contains a quote.
// Returns an empty string on success, "cancelled" when `cancel` was set, or an error message.
std::string run_curl(const std::vector<std::string>& args, const std::atomic<bool>* cancel = nullptr) {
#ifdef _WIN32
    std::wstring command = L"curl.exe";
    for (const auto& arg : args)
        command += L" \"" + fs::u8path(arg).wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
        return "cannot start curl.exe (error " + std::to_string(GetLastError()) + ")";
    CloseHandle(process.hThread);
    while (WaitForSingleObject(process.hProcess, 250) == WAIT_TIMEOUT) {
        if (cancel && cancel->load()) {
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, INFINITE);
            CloseHandle(process.hProcess);
            return "cancelled";
        }
    }
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    return code == 0 ? "" : code == 22 ? "the server returned an HTTP error (e.g. 404 Not Found)" : "curl failed with exit code " + std::to_string(code);
#else
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>("curl"));
    for (const auto& arg : args)
        argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    const pid_t pid = fork();
    if (pid < 0)
        return "cannot start curl";
    if (pid == 0) {
        execvp("curl", argv.data());
        _exit(127);
    }
    int status = 0;
    while (true) {
        const pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid)
            break;
        if (cancel && cancel->load()) {
            kill(pid, SIGTERM);
            waitpid(pid, &status, 0);
            return "cancelled";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return "";
    if (WIFEXITED(status) && WEXITSTATUS(status) == 127)
        return "curl is not installed";
    if (WIFEXITED(status) && WEXITSTATUS(status) == 22)
        return "the server returned an HTTP error (e.g. 404 Not Found)";
    return "curl failed with exit code " + std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : -1);
#endif
}

bool valid_download_url(const std::string& url) {
    static const std::regex pattern(R"(^https?://[A-Za-z0-9.\-]+(:[0-9]+)?/[^\s"'<>\\^`{|}]*$)");
    return url.size() < 4096 && std::regex_match(url, pattern);
}

bool supported_model_extension(const std::string& name, const std::string& category) {
    std::string ext = fs::u8path(name).extension().u8string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    if (category == "tokenizers")
        return ext == ".json";
    return ext == ".gguf" || ext == ".safetensors" || ext == ".sft" || ext == ".ckpt" || ext == ".pt" || ext == ".pth" ||
           ext == ".bin";
}

bool valid_file_name(const std::string& name) {
    static const std::regex pattern(R"(^[A-Za-z0-9][A-Za-z0-9._+\-]{0,200}$)");
    return std::regex_match(name, pattern) && name.find("..") == std::string::npos;
}

// Catalog targets must be "<category>/<name>" inside the model directory, with a model extension.
std::string check_model_path(const std::string& path) {
    const auto slash = path.find('/');
    if (slash == std::string::npos)
        return "path must be <category>/<file>: " + path;
    const std::string category = path.substr(0, slash);
    if (!model_categories().count(category))
        return "unknown model category '" + category + "' in " + path;
    std::string rest = path.substr(slash + 1);
    size_t start     = 0;
    while (true) {
        const auto next  = rest.find('/', start);
        const auto piece = rest.substr(start, next == std::string::npos ? std::string::npos : next - start);
        if (!valid_file_name(piece))
            return "invalid file name in " + path;
        if (next == std::string::npos)
            break;
        start = next + 1;
    }
    if (!supported_model_extension(rest, category))
        return "unsupported file type: " + path;
    return "";
}

std::string check_package(const json& package) {
    static const std::regex id_pattern(R"(^[A-Za-z0-9_.\-]{1,128}$)");
    if (!std::regex_match(package.value("id", ""), id_pattern))
        return "invalid package id";
    for (const auto& file : package.value("files", json::array())) {
        if (!valid_download_url(file.value("url", "")))
            return "invalid URL in package " + package.value("id", "");
        if (auto error = check_model_path(file.value("path", "")); !error.empty())
            return error;
    }
    for (const auto& convert : package.value("convert", json::array())) {
        std::string output = convert.value("output", "");
        if (auto p = output.find("{type}"); p != std::string::npos)
            output.replace(p, 6, "x");
        if (auto error = check_model_path(output); !error.empty())
            return error;
        if (!std::regex_match(convert.value("id", ""), id_pattern))
            return "invalid convert step id";
        for (const auto& input : convert.value("inputs", json::array()))
            if (!valid_file_name(input.value("name", "")) || !valid_download_url(input.value("url", "")))
                return "invalid convert input in package " + package.value("id", "");
    }
    if (package.contains("guide")) {
        const json& guide = package["guide"];
        if (!guide.is_object())
            return "guide must be an object";
        for (const char* key : {"inputs", "alternatives"}) {
            const json files = guide.value(key, json::object());
            if (!files.is_object())
                return std::string("guide ") + key + " must map inputs to files";
            for (auto it = files.begin(); it != files.end(); ++it) {
                const std::string file = it.value().is_string() ? it.value().get<std::string>() : "";
                const bool step        = file.size() > 2 && file.front() == '{' && file.back() == '}';
                if (!step)
                    if (auto error = check_model_path(file); !error.empty())
                        return "guide: " + error;
            }
        }
    }
    return "";
}

std::string read_text_file(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

bool write_text_file(const fs::path& path, const std::string& text) {
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    const fs::path partial = path.u8string() + ".tmp";
    {
        std::ofstream file(partial, std::ios::binary);
        if (!(file << text))
            return false;
    }
    fs::rename(partial, path, error);
    return !error;
}

std::string slugify(std::string text) {
    std::string out;
    for (unsigned char c : text) {
        if (std::isalnum(c))
            out.push_back(static_cast<char>(std::tolower(c)));
        else if (!out.empty() && out.back() != '-')
            out.push_back('-');
    }
    while (!out.empty() && out.back() == '-')
        out.pop_back();
    return out.empty() ? "model" : out.substr(0, 80);
}

struct DownloadFile {
    std::string url;
    std::string sha256;
    uintmax_t size = 0;
    fs::path target;
    std::string label;
};

struct PackageState {
    std::string state = "idle";
    std::string detail;
    std::string error;
    uintmax_t done_bytes  = 0;
    uintmax_t total_bytes = 0;
    fs::path active_part;
    double speed_bps = 0;
};

class DownloadManager {
public:
    void start(ServerRuntime& runtime) {
        runtime_ = &runtime;
        load_catalog();
        worker_ = std::thread([this] { run(); });
        online_ = std::thread([this] { refresh_online_catalog(); });
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!worker_.joinable())
                return;
            stop_ = true;
            cancel_.store(true);
        }
        cv_.notify_all();
        worker_.join();
        if (online_.joinable())
            online_.join();
    }

    // Fetches the online catalog into <model-dir>/downloads/.online-catalog.json. The cached
    // copy keeps working offline; the built-in list applies when there is none.
    json refresh_online_catalog() {
        const std::string url = runtime_->svr_params->catalog_url;
        json status           = {{"url", url}};
        const SDSvrParams& params = *runtime_->svr_params;
        if (url.empty()) {
            status["state"] = "disabled";
        } else if (!params.settings_file.empty() && !params.folders_configured) {
            // The models folder is not chosen yet; do not create a default one next to the app.
            status["state"] = "waiting";
            status["error"] = "choose the models folder first";
        } else if (!valid_download_url(url)) {
            status["state"] = "error";
            status["error"] = "invalid --catalog-url";
        } else {
            const fs::path cache = online_cache_path();
            const fs::path temp  = cache.u8string() + ".tmp";
            std::error_code error;
            fs::create_directories(cache.parent_path(), error);
            const std::string result = run_curl({"-fsSL", "-m", "30", "-o", temp.u8string(), url});
            const json catalog       = result.empty() ? json::parse(read_text_file(temp), nullptr, false) : json();
            if (!result.empty() || !catalog.is_object() || !catalog.contains("packages")) {
                fs::remove(temp, error);
                status["state"] = "error";
                status["error"] = result.empty() ? "the online catalog is not a valid catalog file" : result;
            } else {
                fs::rename(temp, cache, error);
                status["state"] = error ? "error" : "ok";
                if (error)
                    status["error"] = error.message();
            }
        }
        status["checked"] = unix_timestamp_now_ms();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            online_status_ = status;
        }
        load_catalog();
        return status;
    }

    // Adds a single model file from a link as a user package (<model-dir>/downloads/custom-*.json).
    // Hugging Face links get their size and SHA-256 from the Hub API. With dry_run only the
    // details are returned.
    std::pair<int, json> add_custom(const json& body) {
        const std::string category = body.value("category", "");
        std::string url            = body.value("url", "");
        std::string name           = body.value("filename", "");
        if (!model_categories().count(category))
            return {400, {{"error", "choose a model type"}}};
        if (url.rfind("https://", 0) != 0 || !valid_download_url(url))
            return {400, {{"error", "enter an https:// link to the model file"}}};

        static const std::regex hf(R"(^https://huggingface\.co/([^/?#]+)/([^/?#]+)/(?:resolve|blob)/([^/?#]+)/([^?#]+)(?:[?#].*)?$)");
        std::smatch match;
        std::string sha256, homepage;
        uintmax_t size  = 0;
        const fs::path temp = primary_root() / "downloads" / ".inspect.tmp";
        std::error_code error;
        fs::create_directories(temp.parent_path(), error);
        if (std::regex_match(url, match, hf)) {
            const std::string repo = match[1].str() + "/" + match[2].str(), revision = match[3], file = match[4];
            url      = "https://huggingface.co/" + repo + "/resolve/" + revision + "/" + file;
            homepage = "https://huggingface.co/" + repo;
            if (name.empty())
                name = fs::u8path(file).filename().u8string();
            const std::string result = run_curl({"-fsSL", "-m", "30", "-X", "POST", "--data-urlencode", "paths=" + file, "-o",
                                                 temp.u8string(), "https://huggingface.co/api/models/" + repo + "/paths-info/" + revision});
            const json info = result.empty() ? json::parse(read_text_file(temp), nullptr, false) : json();
            fs::remove(temp, error);
            if (!result.empty())
                return {502, {{"error", "Hugging Face did not answer for this file (" + result + "). Gated models that need a login are not supported yet."}}};
            if (!info.is_array() || info.empty() || info[0].value("type", "") != "file")
                return {400, {{"error", "that link is not a file in this Hugging Face repository"}}};
            size = info[0].value("size", uintmax_t(0));
            if (info[0].contains("lfs") && info[0]["lfs"].is_object())
                sha256 = info[0]["lfs"].value("oid", "");
        } else {
            if (name.empty()) {
                std::string path = url.substr(0, url.find_first_of("?#"));
                name             = path.substr(path.find_last_of('/') + 1);
            }
            if (run_curl({"-fsSIL", "-m", "30", "-o", temp.u8string(), url}).empty()) {
                static const std::regex length(R"(content-length:\s*([0-9]+))", std::regex::icase);
                const std::string headers = read_text_file(temp);
                for (std::sregex_iterator it(headers.begin(), headers.end(), length), end; it != end; ++it)
                    size = std::stoull((*it)[1].str());
            }
            fs::remove(temp, error);
        }
        if (!valid_file_name(name) || !supported_model_extension(name, category))
            return {400, {{"error", "the file name must end in .gguf, .safetensors, .sft, .ckpt, .pt, .pth or .bin (.json for tokenizers)"}}};
        const std::string path = category + "/" + name;
        json details           = {{"url", url}, {"path", path}, {"size", size}, {"sha256", sha256}, {"homepage", homepage},
                                  {"exists", file_size_or_zero(primary_root() / fs::u8path(path)) > 0}};
        if (body.value("dry_run", false))
            return {200, details};
        if (details["exists"].get<bool>())
            return {409, {{"error", path + " already exists"}}};

        std::string id = "custom-" + slugify(fs::u8path(name).stem().u8string());
        for (int n = 2; fs::exists(primary_root() / "downloads" / (id + ".json")); ++n)
            id = "custom-" + slugify(fs::u8path(name).stem().u8string()) + "-" + std::to_string(n);
        json file = {{"path", path}, {"url", url}, {"size", size}};
        if (!sha256.empty())
            file["sha256"] = sha256;
        json package = {{"id", id},
                        {"name", body.value("name", "").empty() ? name : body.value("name", "")},
                        {"group", "Added by you"},
                        {"description", "Added from " + url},
                        {"files", json::array({file})}};
        if (!homepage.empty())
            package["homepage"] = homepage;
        if (!write_text_file(primary_root() / "downloads" / (id + ".json"),
                             json({{"schema_version", 1}, {"packages", json::array({package})}}).dump(2) + "\n"))
            return {500, {{"error", "cannot write the package file"}}};
        load_catalog();
        details["id"] = id;
        if (body.value("start", true))
            enqueue(id);
        return {201, details};
    }

    // The full package definition for the catalog editor.
    std::pair<int, json> definition(const std::string& id) {
        load_catalog();
        std::lock_guard<std::mutex> lock(mutex_);
        const json* package = find_package(id);
        if (!package)
            return {404, {{"error", "unknown package"}}};
        json result = *package;
        result.erase("source");
        result.erase("overrides");
        const std::string source = package->value("source", "");
        const bool user_file      = source != "builtin" && source != "online";
        return {200,
                {{"package", result},
                 {"source", source},
                 {"overrides", package->value("overrides", json(nullptr))},
                 {"editable", !user_file || source == id + ".json"}}};
    }

    // Saves a package as <model-dir>/downloads/<id>.json. For a built-in or online package this
    // is an edited copy that replaces the original until it is removed again.
    std::pair<int, json> save_package(const std::string& id, json package) {
        if (!package.is_object() || package.value("id", "") != id)
            return {400, {{"error", "the package id must match the URL"}}};
        if (package.value("name", "").empty())
            return {400, {{"error", "give the package a name"}}};
        for (const char* key : {"source", "overrides", "installed", "size", "missing_bytes", "components", "status"})
            package.erase(key);
        if (!package.contains("files") && !package.contains("convert"))
            return {400, {{"error", "add at least one file"}}};
        if (auto problem = check_package(package); !problem.empty())
            return {400, {{"error", problem}}};
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (const json* existing = find_package(id)) {
                const std::string source = existing->value("source", "");
                if (source != "builtin" && source != "online" && source != id + ".json")
                    return {409, {{"error", "this package comes from " + source + ", which holds several packages; edit that file instead"}}};
            }
            if (active_ == id || queue_position(id) > 0)
                return {409, {{"error", "stop its download first"}}};
        }
        if (!write_text_file(primary_root() / "downloads" / (id + ".json"),
                             json({{"schema_version", 1}, {"packages", json::array({package})}}).dump(2) + "\n"))
            return {500, {{"error", "cannot write the package file"}}};
        load_catalog();
        return {200, {{"saved", true}}};
    }

    // Deletes the downloaded files of a package (its files and packed outputs). Files the loaded
    // model uses are refused; the package stays listed so it can be downloaded again.
    std::pair<int, json> delete_files(const std::string& id) {
        json package;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const json* found = find_package(id);
            if (!found)
                return {404, {{"error", "unknown package"}}};
            if (active_ == id || queue_position(id) > 0)
                return {409, {{"error", "stop its download first"}}};
            package = *found;
        }
        std::vector<std::string> paths;
        for (const auto& component : plan_package(package).components)
            if (component.value("installed", false))
                paths.push_back(component.value("path", ""));
        {
            std::shared_lock<std::shared_mutex> lock(runtime_->models->model_mutex);
            for (auto it = runtime_->models->current_files.begin(); it != runtime_->models->current_files.end(); ++it)
                if (std::find(paths.begin(), paths.end(), it.value().get<std::string>()) != paths.end())
                    return {409, {{"error", it.value().get<std::string>() + " is part of the loaded model; switch to another model first"}}};
        }
        json deleted = json::array();
        for (const auto& path : paths) {
            if (!check_model_path(path).empty())
                continue;
            std::error_code error;
            if (fs::remove(primary_root() / fs::u8path(path), error))
                deleted.push_back(path);
        }
        refresh_model_catalog(*runtime_);
        set_state(id, [](PackageState& s) { s = PackageState{}; });
        return {200, {{"deleted", deleted}}};
    }

    // Removes a package added from the UI (its custom-*.json file). Downloaded files stay.
    std::pair<int, std::string> remove_custom(const std::string& id) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const json* package = find_package(id);
            if (!package)
                return {404, "unknown package"};
            if (package->value("source", "") != id + ".json")
                return {400, "only packages added or edited on this page can be removed or reset"};
            if (active_ == id || queue_position(id) > 0)
                return {409, "stop the download first"};
            states_.erase(id);
        }
        std::error_code error;
        fs::remove(primary_root() / "downloads" / (id + ".json"), error);
        load_catalog();
        return {error ? 500 : 200, error ? error.message() : ""};
    }

    void reload() { load_catalog(); }

    json list() {
        load_catalog();
        std::lock_guard<std::mutex> lock(mutex_);
        json items = json::array();
        for (const auto& package : packages_) {
            const std::string id = package.value("id", "");
            json item            = package;
            item.erase("files");
            item.erase("convert");
            item.erase("recipes");
            if (package.contains("guide"))
                item["guide"] = resolve_guide(package);
            const auto plan   = plan_package(package);
            item["installed"] = plan.missing_bytes == 0 && plan.pending_converts == 0;
            item["size"]           = plan.total_bytes;
            item["missing_bytes"]  = plan.missing_bytes;
            item["components"]     = plan.components;
            const auto& state      = states_[id];
            uintmax_t done         = state.done_bytes;
            if (!state.active_part.empty())
                done += file_size_or_zero(state.active_part);
            item["status"] = {
                {"state", state.state},
                {"detail", state.detail},
                {"error", state.error.empty() ? json(nullptr) : json(state.error)},
                {"done_bytes", done},
                {"total_bytes", state.total_bytes},
                {"speed_bps", state.speed_bps},
                {"queue_position", queue_position(id)},
            };
            items.push_back(std::move(item));
        }
        return {{"packages", items},
                {"errors", errors_},
                {"ram_gb", total_ram_gb()},
                {"model_root", primary_root().u8string()},
                {"catalog", online_status_}};
    }

    std::string enqueue(const std::string& id) {
        load_catalog();
        std::lock_guard<std::mutex> lock(mutex_);
        if (!find_package(id))
            return "unknown package";
        auto& state = states_[id];
        if (state.state == "queued" || active_ == id)
            return "";
        state       = PackageState{};
        state.state = "queued";
        queue_.push_back(id);
        cv_.notify_all();
        return "";
    }

    std::string cancel(const std::string& id) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!find_package(id))
            return "unknown package";
        auto position = std::find(queue_.begin(), queue_.end(), id);
        if (position != queue_.end()) {
            queue_.erase(position);
            states_[id].state = "cancelled";
        } else if (active_ == id) {
            cancel_.store(true);
        }
        return "";
    }

private:
    struct Plan {
        std::vector<DownloadFile> downloads;
        uintmax_t total_bytes   = 0;
        uintmax_t missing_bytes = 0;
        int pending_converts    = 0;
        json components         = json::array();
    };

    ServerRuntime* runtime_ = nullptr;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_;
    std::thread online_;
    json online_status_ = {{"state", "checking"}};
    std::deque<std::string> queue_;
    std::string active_;
    std::atomic<bool> cancel_{false};
    bool stop_ = false;
    std::vector<json> packages_;
    json errors_ = json::array();
    std::map<std::string, PackageState> states_;

    fs::path primary_root() const {
        const auto roots = get_model_roots(*runtime_->svr_params);
        return roots.empty() ? fs::path("models") : roots.front().path;
    }

    fs::path online_cache_path() const { return primary_root() / "downloads" / ".online-catalog.json"; }

    fs::path staging_dir(const std::string& package, const std::string& step) const {
        return primary_root() / "downloads" / ".staging" / package / step;
    }

    int queue_position(const std::string& id) const {
        for (size_t i = 0; i < queue_.size(); ++i)
            if (queue_[i] == id)
                return static_cast<int>(i) + 1;
        return 0;
    }

    const json* find_package(const std::string& id) const {
        for (const auto& package : packages_)
            if (package.value("id", "") == id)
                return &package;
        return nullptr;
    }

    void load_catalog() {
        std::vector<json> packages;
        json errors = json::array();
        auto add    = [&](const json& catalog, const std::string& source) {
            if (!catalog.is_object() || !catalog.contains("packages") || !catalog["packages"].is_array()) {
                errors.push_back({{"file", source}, {"message", "expected an object with a packages array"}});
                return;
            }
            for (const auto& package : catalog["packages"]) {
                if (!package.is_object() || package.value("id", "").empty() || package.value("name", "").empty()) {
                    errors.push_back({{"file", source}, {"message", "each package needs an id and a name"}});
                    continue;
                }
                if (auto problem = check_package(package); !problem.empty()) {
                    errors.push_back({{"file", source}, {"message", problem}});
                    continue;
                }
                auto existing = std::find_if(packages.begin(), packages.end(),
                                             [&](const json& p) { return p["id"] == package["id"]; });
                json item      = package;
                item["source"] = source;
                item.erase("overrides");
                if (existing != packages.end()) {
                    const std::string previous = existing->value("overrides", existing->value("source", ""));
                    if ((previous == "builtin" || previous == "online") && source != "builtin" && source != "online")
                        item["overrides"] = previous;
                    *existing = std::move(item);
                }
                else
                    packages.push_back(std::move(item));
            }
        };
        for (const char* builtin : k_builtin_downloads)
            add(json::parse(builtin), "builtin");
        // Online entries replace built-in ones with the same id; user files replace both.
        if (fs::is_regular_file(online_cache_path()))
            add(json::parse(read_text_file(online_cache_path()), nullptr, false), "online");
        std::error_code error;
        const fs::path user_dir = primary_root() / "downloads";
        if (fs::is_directory(user_dir, error)) {
            for (const auto& entry : fs::directory_iterator(user_dir, error)) {
                if (!entry.is_regular_file() || entry.path().extension() != ".json" ||
                    entry.path().filename().u8string().front() == '.')
                    continue;
                std::ifstream file(entry.path(), std::ios::binary);
                const json catalog = json::parse(file, nullptr, false);
                add(catalog, entry.path().filename().u8string());
            }
        }
        std::lock_guard<std::mutex> lock(mutex_);
        packages_ = std::move(packages);
        errors_   = std::move(errors);
    }

    static std::string pick_type(const json& convert) {
        const double ram = total_ram_gb();
        std::string chosen;
        for (const auto& type : convert.value("types", json::array())) {
            const std::string name = type.value("type", "");
            if (ram >= type.value("min_ram_gb", 0.0))
                return name;
            chosen = name;
        }
        return chosen;
    }

    static std::string output_for(const json& convert, const std::string& type) {
        std::string output    = convert.value("output", "");
        const auto position   = output.find("{type}");
        if (position != std::string::npos)
            output.replace(position, 6, type);
        return output;
    }

    // An existing output of any allowed type satisfies a convert step.
    std::string existing_output(const json& convert) const {
        for (const auto& type : convert.value("types", json::array())) {
            const std::string output = output_for(convert, type.value("type", ""));
            if (file_size_or_zero(primary_root() / fs::u8path(output)) > 0)
                return output;
        }
        return "";
    }

    // A guide's inputs may name a packing step as "{step}"; that becomes the file it produces.
    json resolve_guide(const json& package) const {
        json guide = package.at("guide");
        if (!guide.contains("inputs") || !guide["inputs"].is_object())
            return guide;
        for (auto it = guide["inputs"].begin(); it != guide["inputs"].end(); ++it) {
            if (!it.value().is_string())
                continue;
            const std::string value = it.value().get<std::string>();
            if (value.size() < 3 || value.front() != '{' || value.back() != '}')
                continue;
            for (const auto& convert : package.value("convert", json::array()))
                if (convert.value("id", "") == value.substr(1, value.size() - 2)) {
                    const std::string done = existing_output(convert);
                    it.value()             = done.empty() ? output_for(convert, pick_type(convert)) : done;
                }
        }
        return guide;
    }

    Plan plan_package(const json& package) const {
        Plan plan;
        const fs::path root = primary_root();
        for (const auto& file : package.value("files", json::array())) {
            DownloadFile item{file.value("url", ""), file.value("sha256", ""), file.value("size", uintmax_t(0)),
                              root / fs::u8path(file.value("path", "")), file.value("path", "")};
            // Files of unknown size (links without size information) count as present once they exist.
            const uintmax_t on_disk = file_size_or_zero(item.target);
            const bool present      = item.size > 0 ? on_disk == item.size : on_disk > 0;
            plan.total_bytes += item.size;
            plan.components.push_back({{"path", item.label}, {"size", item.size}, {"installed", present}});
            if (!present) {
                plan.missing_bytes += item.size;
                plan.downloads.push_back(std::move(item));
            }
        }
        for (const auto& convert : package.value("convert", json::array())) {
            const std::string done = existing_output(convert);
            uintmax_t size         = 0;
            for (const auto& input : convert.value("inputs", json::array()))
                size += input.value("size", uintmax_t(0));
            plan.total_bytes += size;
            plan.components.push_back({{"path", done.empty() ? output_for(convert, pick_type(convert)) : done},
                                       {"size", size},
                                       {"installed", !done.empty()},
                                       {"packed_from", convert.value("id", "")}});
            if (done.empty()) {
                plan.missing_bytes += size;
                ++plan.pending_converts;
            }
        }
        return plan;
    }

    void set_state(const std::string& id, const std::function<void(PackageState&)>& update) {
        std::lock_guard<std::mutex> lock(mutex_);
        update(states_[id]);
    }

    std::string fetch(const std::string& id, const DownloadFile& file) {
        std::error_code error;
        fs::create_directories(file.target.parent_path(), error);
        const fs::path part = file.target.string() + ".part";
        set_state(id, [&](PackageState& s) {
            s.state       = "downloading";
            s.detail      = file.label;
            s.active_part = part;
        });
        if (file.size == 0 || file_size_or_zero(part) != file.size) {
            std::atomic<bool> finished{false};
            std::thread meter([&] {
                uintmax_t last = file_size_or_zero(part);
                auto time      = std::chrono::steady_clock::now();
                while (!finished.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                    const uintmax_t now_size = file_size_or_zero(part);
                    const auto now           = std::chrono::steady_clock::now();
                    const double seconds     = std::chrono::duration<double>(now - time).count();
                    const double speed       = seconds > 0 && now_size >= last ? (now_size - last) / seconds : 0;
                    set_state(id, [&](PackageState& s) { s.speed_bps = s.speed_bps * 0.6 + speed * 0.4; });
                    last = now_size, time = now;
                }
            });
            std::string result = run_curl({"-L", "--fail", "--retry", "5", "--retry-delay", "3", "-sS", "-C", "-", "-o",
                                           part.u8string(), file.url},
                                          &cancel_);
            finished.store(true);
            meter.join();
            if (!result.empty())
                return result;
        }
        const uintmax_t size = file_size_or_zero(part);
        if (file.size > 0 && size != file.size) {
            fs::remove(part, error);
            return file.label + ": expected " + std::to_string(file.size) + " bytes, got " + std::to_string(size);
        }
        if (!file.sha256.empty()) {
            set_state(id, [&](PackageState& s) {
                s.state     = "verifying";
                s.speed_bps = 0;
            });
            Sha256 hash;
            std::ifstream input(part, std::ios::binary);
            std::vector<char> buffer(4 << 20);
            while (input) {
                if (cancel_.load())
                    return "cancelled";
                input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                hash.update(reinterpret_cast<const uint8_t*>(buffer.data()), static_cast<size_t>(input.gcount()));
            }
            input.close();
            if (hash.hex() != file.sha256) {
                fs::remove(part, error);
                return file.label + ": checksum mismatch; the partial file was removed, try again";
            }
        }
        fs::rename(part, file.target, error);
        if (error)
            return "cannot move " + file.label + " into place: " + error.message();
        set_state(id, [&](PackageState& s) {
            s.done_bytes += size;
            s.active_part.clear();
        });
        return "";
    }

    std::string install(const std::string& id, const json& package) {
        Plan plan = plan_package(package);
        set_state(id, [&](PackageState& s) {
            s.total_bytes = plan.missing_bytes;
            s.done_bytes  = 0;
        });
        for (const auto& file : plan.downloads)
            if (auto error = fetch(id, file); !error.empty())
                return error;

        std::map<std::string, std::string> outputs;
        for (const auto& convert : package.value("convert", json::array())) {
            const std::string step = convert.value("id", "");
            if (auto done = existing_output(convert); !done.empty()) {
                outputs[step] = done;
                continue;
            }
            const fs::path staging = staging_dir(id, step);
            const auto inputs      = convert.value("inputs", json::array());
            fs::path source;
            for (const auto& input : inputs) {
                DownloadFile file{input.value("url", ""), input.value("sha256", ""), input.value("size", uintmax_t(0)),
                                  staging / fs::u8path(input.value("name", "")), step + "/" + input.value("name", "")};
                if (file_size_or_zero(file.target) != file.size || file.size == 0) {
                    if (auto error = fetch(id, file); !error.empty())
                        return error;
                } else {
                    set_state(id, [&](PackageState& s) { s.done_bytes += file.size; });
                }
                if (fs::path(file.target).extension() == ".safetensors" && source.empty())
                    source = inputs.size() == 1 ? file.target : staging;
            }
            const std::string type   = pick_type(convert);
            const std::string output = output_for(convert, type);
            const fs::path target    = primary_root() / fs::u8path(output);
            const fs::path partial   = target.string() + ".part";
            set_state(id, [&](PackageState& s) {
                s.state     = "packing";
                s.detail    = output + " (" + type + ")";
                s.speed_bps = 0;
            });
            std::error_code error;
            fs::create_directories(target.parent_path(), error);
            const std::string source_text = source.u8string(), partial_text = partial.u8string();
            const bool ok = convert_with_components(source_text.c_str(), "", "", "", "", "", partial_text.c_str(),
                                                    str_to_sd_type(type.c_str()), "", false, 0, nullptr, 0);
            if (!ok) {
                fs::remove(partial, error);
                return "packing " + output + " failed; see the server log";
            }
            fs::rename(partial, target, error);
            if (error)
                return "cannot move " + output + " into place: " + error.message();
            fs::remove_all(staging, error);
            outputs[step] = output;
        }

        std::error_code error;
        fs::remove(primary_root() / "downloads" / ".staging" / id, error);
        return "";
    }

    void run() {
        while (true) {
            std::string id;
            json package;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [&] { return stop_ || !queue_.empty(); });
                if (stop_)
                    return;
                id = queue_.front();
                queue_.pop_front();
                const json* found = find_package(id);
                if (!found)
                    continue;
                package = *found;
                active_ = id;
                cancel_.store(false);
                states_[id].state = "starting";
            }
            std::string error;
            try {
                error = install(id, package);
            } catch (const std::exception& e) {
                error = e.what();
            }
            if (error.empty())
                refresh_model_catalog(*runtime_);
            std::lock_guard<std::mutex> lock(mutex_);
            auto& state = states_[id];
            state.active_part.clear();
            state.speed_bps = 0;
            if (error == "cancelled") {
                state.state = "cancelled";
                state.error.clear();
            } else if (!error.empty()) {
                state.state = "failed";
                state.error = error;
                LOG_WARN("download %s failed: %s", id.c_str(), error.c_str());
            } else {
                state.state  = "completed";
                state.detail = "";
            }
            active_.clear();
        }
    }
};

DownloadManager& manager() {
    static DownloadManager instance;
    return instance;
}

}  // namespace

void register_download_endpoints(httplib::Server& svr, ServerRuntime& runtime) {
    manager().start(runtime);
    svr.Get("/sdcpp/v1/downloads", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(manager().list().dump(), "application/json");
    });
    svr.Post(R"(/sdcpp/v1/downloads/([A-Za-z0-9_.\-]+))", [](const httplib::Request& req, httplib::Response& res) {
        const std::string error = manager().enqueue(req.matches[1]);
        res.status              = error.empty() ? 202 : 404;
        res.set_content(error.empty() ? R"({"queued":true})" : json({{"error", error}}).dump(), "application/json");
    });
    svr.Post(R"(/sdcpp/v1/downloads/([A-Za-z0-9_.\-]+)/cancel)", [](const httplib::Request& req, httplib::Response& res) {
        const std::string error = manager().cancel(req.matches[1]);
        res.status              = error.empty() ? 200 : 404;
        res.set_content(error.empty() ? R"({"cancel_requested":true})" : json({{"error", error}}).dump(), "application/json");
    });
    svr.Delete(R"(/sdcpp/v1/downloads/([A-Za-z0-9_.\-]+))", [](const httplib::Request& req, httplib::Response& res) {
        const auto [status, error] = manager().remove_custom(req.matches[1]);
        res.status                 = status;
        res.set_content(error.empty() ? R"({"removed":true})" : json({{"error", error}}).dump(), "application/json");
    });
    svr.Delete(R"(/sdcpp/v1/downloads/([A-Za-z0-9_.\-]+)/files)", [](const httplib::Request& req, httplib::Response& res) {
        const auto [status, result] = manager().delete_files(req.matches[1]);
        res.status                  = status;
        res.set_content(result.dump(), "application/json");
    });
    svr.Get(R"(/sdcpp/v1/downloads/([A-Za-z0-9_.\-]+)/definition)", [](const httplib::Request& req, httplib::Response& res) {
        const auto [status, result] = manager().definition(req.matches[1]);
        res.status                  = status;
        res.set_content(result.dump(), "application/json");
    });
    svr.Put(R"(/sdcpp/v1/downloads/([A-Za-z0-9_.\-]+))", [](const httplib::Request& req, httplib::Response& res) {
        const auto [status, result] = manager().save_package(req.matches[1], json::parse(req.body, nullptr, false));
        res.status                  = status;
        res.set_content(result.dump(), "application/json");
    });
    svr.Post("/sdcpp/v1/download-catalog/refresh", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(manager().refresh_online_catalog().dump(), "application/json");
    });
    svr.Post("/sdcpp/v1/download-catalog/custom", [](const httplib::Request& req, httplib::Response& res) {
        const json body = json::parse(req.body, nullptr, false);
        if (!body.is_object()) {
            res.status = 400;
            res.set_content(R"({"error":"expected a JSON object"})", "application/json");
            return;
        }
        const auto [status, result] = manager().add_custom(body);
        res.status                  = status;
        res.set_content(result.dump(), "application/json");
    });
}

void stop_downloads() {
    manager().stop();
}

std::string check_download_catalog(const json& catalog) {
    if (!catalog.is_object() || !catalog.contains("packages") || !catalog["packages"].is_array())
        return "expected an object with a packages array";
    for (const auto& package : catalog["packages"]) {
        if (!package.is_object() || package.value("id", "").empty() || package.value("name", "").empty())
            return "each package needs an id and a name";
        if (auto problem = check_package(package); !problem.empty())
            return problem;
    }
    return "";
}

void reload_download_catalog() {
    manager().reload();
}

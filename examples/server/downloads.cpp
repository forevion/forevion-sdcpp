#include "downloads.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
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

// Runs curl for one URL, resuming into `target`. Returns an empty string on success.
std::string run_curl(const std::string& url, const fs::path& target, const std::atomic<bool>& cancel) {
#ifdef _WIN32
    std::wstring command = L"curl.exe -L --fail --retry 5 --retry-delay 3 -sS -C - -o \"" + target.wstring() + L"\" \"" +
                           fs::u8path(url).wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
        return "cannot start curl.exe (error " + std::to_string(GetLastError()) + ")";
    CloseHandle(process.hThread);
    while (WaitForSingleObject(process.hProcess, 250) == WAIT_TIMEOUT) {
        if (cancel.load()) {
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, INFINITE);
            CloseHandle(process.hProcess);
            return "cancelled";
        }
    }
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    return code == 0 ? "" : "curl failed with exit code " + std::to_string(code);
#else
    const std::string output = target.string();
    const pid_t pid          = fork();
    if (pid < 0)
        return "cannot start curl";
    if (pid == 0) {
        execlp("curl", "curl", "-L", "--fail", "--retry", "5", "--retry-delay", "3", "-sS", "-C", "-", "-o",
               output.c_str(), url.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    int status = 0;
    while (true) {
        const pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid)
            break;
        if (cancel.load()) {
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
    return "curl failed with exit code " + std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : -1);
#endif
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
    }

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
        return {{"packages", items}, {"errors", errors_}, {"ram_gb", total_ram_gb()}, {"model_root", primary_root().u8string()}};
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
                auto existing = std::find_if(packages.begin(), packages.end(),
                                             [&](const json& p) { return p["id"] == package["id"]; });
                json item      = package;
                item["source"] = source;
                if (existing != packages.end())
                    *existing = std::move(item);
                else
                    packages.push_back(std::move(item));
            }
        };
        for (const char* builtin : k_builtin_downloads)
            add(json::parse(builtin), "builtin");
        std::error_code error;
        const fs::path user_dir = primary_root() / "downloads";
        if (fs::is_directory(user_dir, error)) {
            for (const auto& entry : fs::directory_iterator(user_dir, error)) {
                if (!entry.is_regular_file() || entry.path().extension() != ".json")
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

    Plan plan_package(const json& package) const {
        Plan plan;
        const fs::path root = primary_root();
        for (const auto& file : package.value("files", json::array())) {
            DownloadFile item{file.value("url", ""), file.value("sha256", ""), file.value("size", uintmax_t(0)),
                              root / fs::u8path(file.value("path", "")), file.value("path", "")};
            const bool present = file_size_or_zero(item.target) == item.size && item.size > 0;
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
            std::string result = run_curl(file.url, part, cancel_);
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

        for (auto recipe : package.value("recipes", json::array())) {
            if (recipe.contains("models"))
                for (auto it = recipe["models"].begin(); it != recipe["models"].end(); ++it)
                    if (auto& file = it.value(); file.is_string()) {
                        std::string value = file.get<std::string>();
                        if (value.size() > 2 && value.front() == '{' && value.back() == '}' &&
                            outputs.count(value.substr(1, value.size() - 2)))
                            file = outputs[value.substr(1, value.size() - 2)];
                    }
            const fs::path path = primary_root() / "recipes" / (recipe.value("id", "recipe") + ".json");
            std::error_code error;
            if (fs::exists(path, error))
                continue;
            fs::create_directories(path.parent_path(), error);
            std::ofstream out(path, std::ios::binary);
            out << recipe.dump(2) << "\n";
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
}

void stop_downloads() {
    manager().stop();
}

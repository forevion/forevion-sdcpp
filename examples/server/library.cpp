#include "httplib.h"
#include "library.h"

#include <algorithm>
#include <ctime>
#include <fstream>
#include <regex>
#include <sstream>

#include "async_jobs.h"
#include "common/log.h"

namespace fs = std::filesystem;

namespace {

bool decode_base64(const std::string& input, std::string& output) {
    static const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    output.clear();
    output.reserve(input.size() * 3 / 4);
    uint32_t buffer = 0;
    int bits        = 0;
    for (char c : input) {
        if (c == '=' || c == '\n' || c == '\r')
            continue;
        const size_t value = alphabet.find(c);
        if (value == std::string::npos)
            return false;
        buffer = (buffer << 6) | static_cast<uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            output.push_back(static_cast<char>((buffer >> bits) & 0xFF));
        }
    }
    return true;
}

std::string mime_for_extension(const std::string& extension) {
    if (extension == ".png")
        return "image/png";
    if (extension == ".jpg" || extension == ".jpeg")
        return "image/jpeg";
    if (extension == ".webp")
        return "image/webp";
    if (extension == ".webm")
        return "video/webm";
    if (extension == ".avi")
        return "video/x-msvideo";
    if (extension == ".wav")
        return "audio/wav";
    return "application/octet-stream";
}

bool valid_output_name(const std::string& name) {
    static const std::regex pattern(R"(^[A-Za-z0-9_\-]+\.[A-Za-z0-9]+$)");
    return std::regex_match(name, pattern);
}

fs::path output_dir(const ServerRuntime& runtime) {
    return fs::u8path(runtime.svr_params->output_dir);
}

std::string local_timestamp(int64_t unix_ms) {
    const std::time_t seconds = static_cast<std::time_t>(unix_ms / 1000);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &seconds);
#else
    localtime_r(&seconds, &tm);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y%m%d-%H%M%S", &tm);
    return buffer;
}

bool write_file(const fs::path& path, const std::string& bytes) {
    const fs::path partial = path.string() + ".tmp";
    {
        std::ofstream file(partial, std::ios::binary);
        if (!file)
            return false;
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!file)
            return false;
    }
    std::error_code error;
    fs::rename(partial, path, error);
    return !error;
}

// Extension for an uploaded data URL or bare Base64 payload, from its MIME type or first bytes.
std::string input_extension(const std::string& mime, const std::string& bytes) {
    if (mime == "image/jpeg" || (bytes.size() > 2 && static_cast<unsigned char>(bytes[0]) == 0xFF && static_cast<unsigned char>(bytes[1]) == 0xD8))
        return ".jpg";
    if (mime == "image/webp" || (bytes.size() > 12 && bytes.compare(8, 4, "WEBP") == 0))
        return ".webp";
    if (mime.rfind("audio/", 0) == 0 || bytes.rfind("RIFF", 0) == 0)
        return ".wav";
    return ".png";
}

// Saves one uploaded input under a name made from its content, so the same picture is kept once.
std::string save_input(const fs::path& dir, const std::string& value) {
    std::string mime;
    std::string payload = value;
    if (value.rfind("data:", 0) == 0) {
        const size_t comma = value.find(',');
        if (comma == std::string::npos)
            return "";
        mime    = value.substr(5, value.find(';') < comma ? value.find(';') - 5 : comma - 5);
        payload = value.substr(comma + 1);
    }
    std::string bytes;
    if (!decode_base64(payload, bytes) || bytes.empty())
        return "";
    uint64_t hash = 1469598103934665603ULL;
    for (unsigned char c : bytes)
        hash = (hash ^ c) * 1099511628211ULL;
    char name[40];
    snprintf(name, sizeof(name), "input-%016llx", static_cast<unsigned long long>(hash));
    const std::string file = name + input_extension(mime, bytes);
    std::error_code error;
    if (!fs::exists(dir / file, error) && !write_file(dir / file, bytes))
        return "";
    return file;
}

template <typename T>
json first_set(const std::optional<T>& value, const std::optional<T>& fallback) {
    if (value)
        return *value;
    if (fallback)
        return *fallback;
    return nullptr;
}

// Random seeds are resolved inside the generation; its DGL info records the value used.
std::string seed_from_generation_info(const json& info) {
    const std::string text = info.is_string() ? info.get<std::string>() : info.dump();
    static const std::regex pattern(R"(\bseed = (-?\d+))");
    std::smatch match;
    return std::regex_search(text, match, pattern) ? match[1].str() : "";
}

json read_json_file(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return json::parse(file, nullptr, false);
}

}  // namespace

json make_job_summary(const AsyncGenerationJob& job, const json& body) {
    const bool video          = job.kind == AsyncJobKind::VidGen;
    const auto& params        = video ? job.vid_gen.gen_params : job.img_gen.gen_params;
    const SDGenerationParams* defaults = job.model ? &job.model->defaults : nullptr;
    auto pick                 = [&](auto member) -> json {
        const auto& value = params.*member;
        if (value)
            return *value;
        if (defaults && defaults->*member)
            return *(defaults->*member);
        return nullptr;
    };
    json summary = {
        {"kind", async_job_kind_name(job.kind)},
        {"prompt", params.prompt},
        {"negative_prompt", params.negative_prompt},
        {"width", pick(&SDGenerationParams::width)},
        {"height", pick(&SDGenerationParams::height)},
        {"seed", pick(&SDGenerationParams::seed)},
        {"steps", first_set(params.sample_params.sample_steps,
                            defaults ? defaults->sample_params.sample_steps : std::optional<int>{})},
    };
    if (video) {
        summary["video_frames"] = pick(&SDGenerationParams::video_frames);
        summary["fps"]          = pick(&SDGenerationParams::fps);
    } else {
        summary["batch_count"] = pick(&SDGenerationParams::batch_count);
    }
    if (auto model = body.find("model"); model != body.end() && model->is_object()) {
        summary["recipe"] = model->value("recipe", "");
        summary["models"] = model->value("models", json::object());
    } else {
        summary["recipe"] = "@current";
    }
    if (auto loras = body.find("lora"); loras != body.end() && loras->is_array()) {
        json entries = json::array();
        for (const auto& item : *loras)
            if (item.is_object() && item.contains("path") && item["path"].is_string())
                entries.push_back({{"path", item["path"]},
                                   {"multiplier", item.value("multiplier", 1.0f)},
                                   {"is_high_noise", item.value("is_high_noise", false)}});
        if (!entries.empty())
            summary["lora"] = std::move(entries);
    }
    return summary;
}

json save_job_inputs(const ServerRuntime& runtime, const json& body) {
    json saved = json::object();
    if (runtime.svr_params->input_dir.empty() || !body.is_object())
        return saved;
    const fs::path dir = fs::u8path(runtime.svr_params->input_dir);
    std::error_code error;
    fs::create_directories(dir, error);
    auto url = [](const std::string& file) { return "/sdcpp/v1/inputs/files/" + file; };
    for (const char* key : {"init_image", "end_image"}) {
        const auto it = body.find(key);
        if (it != body.end() && it->is_string())
            if (const auto file = save_input(dir, it->get<std::string>()); !file.empty())
                saved[key] = url(file);
    }
    for (const char* key : {"ref_images", "ref_audios", "control_frames"}) {
        const auto it = body.find(key);
        if (it == body.end() || !it->is_array())
            continue;
        json files = json::array();
        for (const auto& item : *it)
            if (item.is_string())
                if (const auto file = save_input(dir, item.get<std::string>()); !file.empty())
                    files.push_back(url(file));
        if (!files.empty())
            saved[key] = files;
    }
    if (!saved.empty())
        LOG_INFO("saved %zu uploaded input(s) in %s", saved.size(), dir.u8string().c_str());
    return saved;
}

void save_job_outputs(const ServerRuntime& runtime, const AsyncGenerationJob& job) {
    if (runtime.svr_params->output_dir.empty() || job.kind == AsyncJobKind::Dgl)
        return;
    const fs::path dir = output_dir(runtime);
    std::error_code error;
    fs::create_directories(dir, error);
    if (error) {
        LOG_WARN("cannot create output directory %s: %s", dir.u8string().c_str(), error.message().c_str());
        return;
    }

    const std::string sequence = job.id.substr(job.id.find_last_of('_') + 1);
    const std::string base     = local_timestamp(job.created_at) + "_" + sequence;
    json meta                  = job.summary.is_object() ? job.summary : json::object();
    meta["job_id"]             = job.id;
    meta["created"]            = job.created_at;
    meta["started"]            = job.started_at;
    meta["completed"]          = job.completed_at;
    meta["elapsed_ms"]         = job.completed_at - job.started_at;
    {
        std::lock_guard<std::mutex> lock(job.progress_mutex);
        meta["stage_ms"] = job.progress.stage_ms;
    }

    auto save = [&](const std::string& name, const std::string& b64, json item_meta) {
        std::string bytes;
        if (!decode_base64(b64, bytes)) {
            LOG_WARN("cannot decode output %s", name.c_str());
            return;
        }
        item_meta["file"] = name;
        item_meta["mime_type"] = mime_for_extension(fs::path(name).extension().string());
        item_meta["bytes"] = bytes.size();
        if (item_meta.value("seed", json(-1)) == json(-1) && item_meta.contains("generation_info"))
            if (auto seed = seed_from_generation_info(item_meta["generation_info"]); !seed.empty())
                item_meta["seed"] = std::stoll(seed);
        if (!write_file(dir / name, bytes) || !write_file(dir / (fs::path(name).stem().string() + ".json"), item_meta.dump(2)))
            LOG_WARN("cannot write output %s", (dir / name).u8string().c_str());
        else
            LOG_INFO("job %s: saved %s", job.id.c_str(), (dir / name).u8string().c_str());
    };

    if (job.kind == AsyncJobKind::VidGen) {
        json item           = meta;
        item["frame_count"] = job.result_frame_count;
        item["fps"]         = job.result_fps;
        item["generation_info"] = job.result_generation_info;
        const std::string extension = job.result_media_mime_type == "image/png" ? "png" : job.vid_gen.output_format;
        if (!job.result_preview_b64.empty()) {
            std::string preview;
            if (decode_base64(job.result_preview_b64, preview) && write_file(dir / (base + "-preview.webm"), preview)) {
                item["preview_file"] = base + "-preview.webm";
                item["preview_url"]  = "/sdcpp/v1/outputs/files/" + base + "-preview.webm";
            }
            item["has_audio"] = true;
        }
        if (!job.result_audio_b64.empty()) {
            std::string audio;
            if (decode_base64(job.result_audio_b64, audio) && write_file(dir / (base + "-audio.wav"), audio)) {
                item["audio_file"] = base + "-audio.wav";
                item["audio_url"]  = "/sdcpp/v1/outputs/files/" + base + "-audio.wav";
            }
        }
        if (job.vid_gen.still_frame >= 0) {
            item["rendered_frames"] = item.value("video_frames", json(nullptr));
            item["video_frames"]    = 1;
        }
        save(base + "." + extension, job.result_media_b64, std::move(item));
        return;
    }
    const std::string extension = job.img_gen.output_format == "jpeg" ? "jpg" : job.img_gen.output_format;
    for (size_t i = 0; i < job.result_images_b64.size(); ++i) {
        json item = meta;
        item["kind"] = "img_gen";
        item["index"] = i;
        if (job.result_generation_info.is_array() && i < job.result_generation_info.size())
            item["generation_info"] = job.result_generation_info[i];
        const std::string name = job.result_images_b64.size() > 1 ? base + "-" + std::to_string(i) : base;
        save(name + "." + extension, job.result_images_b64[i], std::move(item));
    }
}

void register_library_endpoints(httplib::Server& svr, ServerRuntime& runtime) {
    ServerRuntime* rt = &runtime;

    svr.Get("/sdcpp/v1/jobs", [rt](const httplib::Request&, httplib::Response& res) {
        AsyncJobManager& manager = *rt->async_job_manager;
        std::lock_guard<std::mutex> lock(manager.mutex);
        purge_expired_jobs(manager);
        std::vector<std::shared_ptr<AsyncGenerationJob>> jobs;
        for (const auto& entry : manager.jobs)
            if (entry.second->kind != AsyncJobKind::Dgl)
                jobs.push_back(entry.second);
        std::sort(jobs.begin(), jobs.end(), [](const auto& a, const auto& b) { return a->created_at < b->created_at; });
        json items = json::array();
        for (const auto& job : jobs) {
            json item = make_async_job_json(manager, *job);
            item.erase("result");
            item.erase("preview");
            item["summary"] = job->summary;
            items.push_back(std::move(item));
        }
        res.set_content(json({{"jobs", items}}).dump(), "application/json");
    });

    svr.Get("/sdcpp/v1/outputs", [rt](const httplib::Request& req, httplib::Response& res) {
        json items = json::array();
        const fs::path dir = output_dir(*rt);
        std::error_code error;
        if (!rt->svr_params->output_dir.empty() && fs::is_directory(dir, error)) {
            std::vector<json> found;
            for (const auto& entry : fs::directory_iterator(dir, error)) {
                if (!entry.is_regular_file() || entry.path().extension() != ".json")
                    continue;
                json meta = read_json_file(entry.path());
                if (!meta.is_object() || !meta.contains("file"))
                    continue;
                const std::string file = meta.value("file", "");
                if (!valid_output_name(file) || !fs::exists(dir / file))
                    continue;
                meta["url"] = "/sdcpp/v1/outputs/files/" + file;
                found.push_back(std::move(meta));
            }
            std::sort(found.begin(), found.end(), [](const json& a, const json& b) {
                const auto ta = a.value("created", int64_t(0)), tb = b.value("created", int64_t(0));
                return ta != tb ? ta > tb : a.value("file", "") > b.value("file", "");
            });
            size_t offset = req.has_param("offset") ? std::stoul(req.get_param_value("offset")) : 0;
            size_t limit  = req.has_param("limit") ? std::stoul(req.get_param_value("limit")) : found.size();
            for (size_t i = offset; i < found.size() && i < offset + limit; ++i)
                items.push_back(std::move(found[i]));
            res.set_content(json({{"outputs", items}, {"total", found.size()}, {"enabled", true}}).dump(), "application/json");
            return;
        }
        res.set_content(json({{"outputs", items}, {"total", 0}, {"enabled", !rt->svr_params->output_dir.empty()}}).dump(), "application/json");
    });

    svr.Get(R"(/sdcpp/v1/outputs/files/([A-Za-z0-9_\-]+\.[A-Za-z0-9]+))", [rt](const httplib::Request& req, httplib::Response& res) {
        const std::string name = req.matches[1];
        const fs::path path     = output_dir(*rt) / name;
        std::ifstream file(path, std::ios::binary);
        if (rt->svr_params->output_dir.empty() || !valid_output_name(name) || !file) {
            res.status = 404;
            res.set_content(R"({"error":"output not found"})", "application/json");
            return;
        }
        std::ostringstream bytes;
        bytes << file.rdbuf();
        res.set_header("Cache-Control", "private, max-age=31536000, immutable");
        res.set_content(bytes.str(), mime_for_extension(path.extension().string()));
    });

    svr.Get(R"(/sdcpp/v1/inputs/files/(input-[0-9a-f]+\.[a-z]+))", [rt](const httplib::Request& req, httplib::Response& res) {
        const fs::path path = fs::u8path(rt->svr_params->input_dir) / std::string(req.matches[1]);
        std::ifstream file(path, std::ios::binary);
        if (rt->svr_params->input_dir.empty() || !file) {
            res.status = 404;
            res.set_content(R"({"error":"input not found"})", "application/json");
            return;
        }
        std::ostringstream bytes;
        bytes << file.rdbuf();
        res.set_header("Cache-Control", "private, max-age=31536000, immutable");
        res.set_content(bytes.str(), mime_for_extension(path.extension().string()));
    });

    svr.Delete(R"(/sdcpp/v1/outputs/([A-Za-z0-9_\-]+\.[A-Za-z0-9]+))", [rt](const httplib::Request& req, httplib::Response& res) {
        const std::string name = req.matches[1];
        if (rt->svr_params->output_dir.empty() || !valid_output_name(name) || fs::path(name).extension() == ".json") {
            res.status = 404;
            res.set_content(R"({"error":"output not found"})", "application/json");
            return;
        }
        const fs::path dir = output_dir(*rt);
        std::error_code error;
        const bool removed = fs::remove(dir / name, error);
        fs::remove(dir / (fs::path(name).stem().string() + ".json"), error);
        fs::remove(dir / (fs::path(name).stem().string() + "-preview.webm"), error);
        fs::remove(dir / (fs::path(name).stem().string() + "-audio.wav"), error);
        res.status = removed ? 200 : 404;
        res.set_content(json({{"deleted", removed}}).dump(), "application/json");
    });
}

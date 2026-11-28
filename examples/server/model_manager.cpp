// Endpoints for the Web UI's model manager: slot definitions, saving and deleting user
// recipes, and listing or deleting installed model files.
#include "httplib.h"
#include "model_manager.h"

#include <fstream>
#include <regex>

#include "common/log.h"
#include "downloads.h"
#include "model_catalog.h"

namespace fs = std::filesystem;

namespace {

void reply(httplib::Response& res, int status, const json& body) {
    res.status = status;
    res.set_content(body.dump(), "application/json");
}

void reply_error(httplib::Response& res, int status, const std::string& message) {
    reply(res, status, {{"error", message}});
}

fs::path primary_root(const ServerRuntime& runtime) {
    const auto roots = get_model_roots(*runtime.svr_params);
    return roots.empty() ? fs::path("models") : roots.front().path;
}

bool write_json_file(const fs::path& path, const json& value) {
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    const fs::path partial = path.u8string() + ".tmp";
    {
        std::ofstream file(partial, std::ios::binary);
        if (!(file << value.dump(2) << "\n"))
            return false;
    }
    fs::rename(partial, path, error);
    return !error;
}

int64_t file_time_ms(const fs::path& path) {
    std::error_code error;
    const auto time = fs::last_write_time(path, error);
    if (error)
        return 0;
    const auto system = std::chrono::time_point_cast<std::chrono::milliseconds>(
        time - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    return system.time_since_epoch().count();
}

}  // namespace

void register_model_manager_endpoints(httplib::Server& svr, ServerRuntime& runtime) {
    ServerRuntime* rt = &runtime;

    svr.Get("/sdcpp/v1/model-slots", [](const httplib::Request&, httplib::Response& res) {
        reply(res, 200, {{"slots", model_slot_definitions()}, {"categories", model_categories()}});
    });

    // Creates or replaces a user recipe. Built-in recipes stay unchanged; saving one with the
    // same id adds a user recipe that overrides it.
    svr.Put(R"(/sdcpp/v1/recipes/([A-Za-z0-9_.\-]{1,128}))", [rt](const httplib::Request& req, httplib::Response& res) {
        json recipe = json::parse(req.body, nullptr, false);
        if (!recipe.is_object())
            return reply_error(res, 400, "expected a recipe object");
        const std::string id = req.matches[1];
        if (recipe.value("id", "") != id)
            return reply_error(res, 400, "the recipe id must match the URL");
        try {
            recipe = prepare_user_recipe(std::move(recipe));
        } catch (const std::exception& error) {
            return reply_error(res, 400, std::string("invalid recipe: ") + error.what());
        }
        fs::path path = primary_root(*rt) / "recipes" / (id + ".json");
        {
            std::lock_guard<std::mutex> lock(rt->models->catalog_mutex);
            const auto existing = rt->models->recipes.find(id);
            if (existing != rt->models->recipes.end() && existing->second.value("source", "") == "user") {
                const auto roots = get_model_roots(*rt->svr_params);
                for (const auto& root : roots)
                    if (root.id == existing->second.value("root", ""))
                        path = root.path / "recipes" / existing->second.value("file", id + ".json");
            }
        }
        if (!write_json_file(path, recipe))
            return reply_error(res, 500, "cannot write " + path.u8string());
        refresh_model_catalog(*rt);
        std::lock_guard<std::mutex> lock(rt->models->catalog_mutex);
        json errors = json::array();
        for (const auto& error : rt->models->catalog_errors)
            if (error.value("file", "").find(path.filename().u8string()) != std::string::npos)
                errors.push_back(error);
        reply(res, errors.empty() ? 200 : 400, {{"saved", errors.empty()}, {"file", path.u8string()}, {"errors", errors}});
    });

    svr.Delete(R"(/sdcpp/v1/recipes/([A-Za-z0-9_.\-]{1,128}))", [rt](const httplib::Request& req, httplib::Response& res) {
        const std::string id = req.matches[1];
        fs::path path;
        {
            std::lock_guard<std::mutex> lock(rt->models->catalog_mutex);
            const auto existing = rt->models->recipes.find(id);
            if (existing == rt->models->recipes.end())
                return reply_error(res, 404, "unknown recipe");
            if (existing->second.value("source", "") != "user")
                return reply_error(res, 400, "built-in recipes cannot be deleted");
            for (const auto& root : get_model_roots(*rt->svr_params))
                if (root.id == existing->second.value("root", ""))
                    path = root.path / "recipes" / existing->second.value("file", "");
        }
        std::error_code error;
        if (path.empty() || !fs::remove(path, error))
            return reply_error(res, 500, "cannot delete the recipe file");
        refresh_model_catalog(*rt);
        reply(res, 200, {{"deleted", true}});
    });

    // Every model file with its size, the recipes that use it, and free space per model root.
    svr.Get("/sdcpp/v1/storage", [rt](const httplib::Request&, httplib::Response& res) {
        json in_use = json::object();
        {
            std::shared_lock<std::shared_mutex> lock(rt->models->model_mutex);
            in_use = rt->models->current_files;
        }
        std::lock_guard<std::mutex> lock(rt->models->catalog_mutex);
        std::map<std::string, json> used_by;
        for (const auto& [recipe_id, recipe] : rt->models->recipes) {
            const json defaults = recipe.value("models", json::object());
            for (auto it = defaults.begin(); it != defaults.end(); ++it)
                if (it.value().is_string())
                    used_by[it.value().get<std::string>()].push_back({{"id", recipe_id}, {"name", recipe.value("name", recipe_id)}});
        }
        json files = json::array();
        for (const auto& [id, file] : rt->models->files) {
            const fs::path path = file.root.path / fs::u8path(file.relative_path);
            std::error_code error;
            const auto size = fs::file_size(path, error);
            bool loaded     = false;
            for (auto it = in_use.begin(); it != in_use.end(); ++it)
                loaded |= it.value() == id;
            files.push_back({{"id", id},
                             {"name", file.name},
                             {"category", file.category},
                             {"size", error ? 0 : size},
                             {"modified", file_time_ms(path)},
                             {"used_by", used_by.count(id) ? used_by[id] : json::array()},
                             {"loaded", loaded}});
        }
        json roots = json::array();
        for (const auto& root : get_model_roots(*rt->svr_params)) {
            std::error_code error;
            const auto space = fs::space(root.path, error);
            roots.push_back({{"id", root.id},
                             {"path", root.path.u8string()},
                             {"free_bytes", error ? 0 : space.available},
                             {"total_bytes", error ? 0 : space.capacity}});
        }
        reply(res, 200, {{"files", files}, {"roots", roots}});
    });

    // Backup of the user's model configuration: presets (<model-dir>/recipes/*.json) and download
    // packages (<model-dir>/downloads/*.json). Model files are not included.
    svr.Get("/sdcpp/v1/config/backup", [rt](const httplib::Request&, httplib::Response& res) {
        const fs::path root = primary_root(*rt);
        json backup         = {{"kind", "sdcpp-studio-config"}, {"version", 1}, {"created", unix_timestamp_now_ms()},
                               {"presets", json::array()}, {"packages", json::array()}};
        for (const auto& [dir, key] : {std::pair<const char*, const char*>{"recipes", "presets"}, {"downloads", "packages"}}) {
            std::error_code error;
            for (const auto& entry : fs::directory_iterator(root / dir, error)) {
                const std::string name = entry.path().filename().u8string();
                if (!entry.is_regular_file() || entry.path().extension() != ".json" || name.front() == '.')
                    continue;
                std::ifstream file(entry.path(), std::ios::binary);
                json content = json::parse(file, nullptr, false);
                if (!content.is_discarded())
                    backup[key].push_back({{"file", name}, {"content", content}});
            }
        }
        res.set_header("Content-Disposition", "attachment; filename=\"sdcpp-config.json\"");
        reply(res, 200, backup);
    });

    // Restores a backup. "merge" writes its files over existing ones with the same name;
    // "replace" first removes the current presets and packages. Nothing is written when any
    // entry is invalid.
    svr.Post("/sdcpp/v1/config/restore", [rt](const httplib::Request& req, httplib::Response& res) {
        const json body = json::parse(req.body, nullptr, false);
        if (!body.is_object())
            return reply_error(res, 400, "expected a JSON object");
        const json backup      = body.value("backup", json());
        const std::string mode = body.value("mode", "merge");
        if (mode != "merge" && mode != "replace")
            return reply_error(res, 400, "mode must be merge or replace");
        if (!backup.is_object() || backup.value("kind", "") != "sdcpp-studio-config" || backup.value("version", 0) != 1)
            return reply_error(res, 400, "this is not an sd.cpp Studio config backup");

        static const std::regex file_name(R"(^[A-Za-z0-9][A-Za-z0-9_.\-]{0,150}\.json$)");
        json errors = json::array();
        std::vector<std::pair<fs::path, json>> writes;
        const fs::path root = primary_root(*rt);
        for (const auto& item : backup.value("presets", json::array())) {
            const std::string name = item.value("file", "");
            try {
                if (!std::regex_match(name, file_name))
                    throw std::runtime_error("invalid file name");
                writes.emplace_back(root / "recipes" / name, prepare_user_recipe(item.value("content", json())));
            } catch (const std::exception& error) {
                errors.push_back({{"file", "recipes/" + name}, {"message", error.what()}});
            }
        }
        for (const auto& item : backup.value("packages", json::array())) {
            const std::string name = item.value("file", "");
            const json content     = item.value("content", json());
            std::string problem    = std::regex_match(name, file_name) ? check_download_catalog(content) : "invalid file name";
            if (problem.empty())
                writes.emplace_back(root / "downloads" / name, content);
            else
                errors.push_back({{"file", "downloads/" + name}, {"message", problem}});
        }
        if (!errors.empty())
            return reply(res, 400, {{"error", "the backup has invalid entries; nothing was changed"}, {"errors", errors}});

        int removed = 0;
        if (mode == "replace") {
            for (const char* dir : {"recipes", "downloads"}) {
                std::error_code error;
                for (const auto& entry : fs::directory_iterator(root / dir, error)) {
                    const std::string name = entry.path().filename().u8string();
                    if (entry.is_regular_file() && entry.path().extension() == ".json" && name.front() != '.')
                        removed += fs::remove(entry.path(), error) ? 1 : 0;
                }
            }
        }
        int written = 0;
        for (const auto& [path, content] : writes)
            written += write_json_file(path, content) ? 1 : 0;
        refresh_model_catalog(*rt);
        reload_download_catalog();
        reply(res, 200, {{"written", written}, {"removed", removed}, {"mode", mode}});
    });

    // Deletes a model file from disk. Files the loaded model uses are refused.
    svr.Delete(R"(/sdcpp/v1/model-files/(.+))", [rt](const httplib::Request& req, httplib::Response& res) {
        const std::string id = req.matches[1];
        {
            std::shared_lock<std::shared_mutex> lock(rt->models->model_mutex);
            for (auto it = rt->models->current_files.begin(); it != rt->models->current_files.end(); ++it)
                if (it.value() == id)
                    return reply_error(res, 409, "this file is part of the loaded model; switch to another model first");
        }
        fs::path path;
        {
            std::lock_guard<std::mutex> lock(rt->models->catalog_mutex);
            const auto file = rt->models->files.find(id);
            if (file == rt->models->files.end())
                return reply_error(res, 404, "unknown model file");
            const fs::path candidate = file->second.root.path / fs::u8path(file->second.relative_path);
            std::error_code error;
            const fs::path canonical = fs::canonical(candidate, error);
            if (error || !model_path_inside(file->second.root.path, canonical))
                return reply_error(res, 400, "the file is outside the model directory");
            path = canonical;
        }
        std::error_code error;
        if (!fs::remove(path, error))
            return reply_error(res, 500, "cannot delete the file: " + error.message());
        LOG_INFO("deleted model file %s", path.u8string().c_str());
        refresh_model_catalog(*rt);
        reply(res, 200, {{"deleted", true}});
    });
}

#include "httplib.h"
#include "studio_settings.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>

#include "async_jobs.h"
#include "common/log.h"
#include "model_catalog.h"

namespace fs = std::filesystem;

namespace {

const char* const k_folder_keys[] = {"models", "inputs", "outputs"};

void reply(httplib::Response& res, int status, const json& body) {
    res.status = status;
    res.set_content(body.dump(), "application/json");
}

void reply_error(httplib::Response& res, int status, const std::string& message) {
    reply(res, status, {{"error", message}});
}

json read_settings(const std::string& file) {
    if (file.empty())
        return json::object();
    std::ifstream in(fs::u8path(file), std::ios::binary);
    if (!in)
        return json::object();
    json settings = json::parse(in, nullptr, false);
    return settings.is_object() ? settings : json::object();
}

std::string folder_setting(const json& settings, const char* key) {
    const auto it = settings.find(std::string(key) + "_dir");
    return it != settings.end() && it->is_string() ? it->get<std::string>() : "";
}

bool is_directory(const std::string& path) {
    std::error_code error;
    return !path.empty() && fs::is_directory(fs::u8path(path), error);
}

std::string absolute_path(const std::string& path) {
    if (path.empty())
        return "";
    std::error_code error;
    const auto absolute = fs::absolute(fs::u8path(path), error);
    return error ? path : absolute.lexically_normal().u8string();
}

std::string current_folder(const SDSvrParams& params, const std::string& key) {
    if (key == "models")
        return params.model_dirs.empty() ? "models" : params.model_dirs.front();
    return key == "inputs" ? params.input_dir : params.output_dir;
}

// The settings file's folder, or the working directory: suggested parent for new folders.
fs::path base_folder(const SDSvrParams& params) {
    std::error_code error;
    if (!params.settings_file.empty()) {
        const auto parent = fs::absolute(fs::u8path(params.settings_file), error).parent_path();
        if (!error)
            return parent;
    }
    return fs::current_path(error);
}

fs::path home_folder() {
#ifdef _WIN32
    const char* home = std::getenv("USERPROFILE");
#else
    const char* home = std::getenv("HOME");
#endif
    return home && *home ? fs::u8path(home) : fs::current_path();
}

// Browsing and creating folders shows this computer's file system, so only the computer itself may do it.
bool from_this_computer(const httplib::Request& req) {
    const std::string& ip = req.remote_addr;
    return ip == "::1" || ip.rfind("127.", 0) == 0 || ip.rfind("::ffff:127.", 0) == 0;
}

// Hidden and system folders (dot folders, $Recycle.Bin, System Volume Information, ...) are not offered.
bool hidden_folder(const fs::path& path) {
    const std::string name = path.filename().u8string();
    if (name.empty() || name.front() == '.' || name.front() == '$')
        return true;
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM));
#else
    return false;
#endif
}

json folder_roots() {
    json roots = json::array();
#ifdef _WIN32
    for (char drive = 'A'; drive <= 'Z'; ++drive) {
        const std::string path = std::string(1, drive) + ":\\";
        std::error_code error;
        if (fs::is_directory(path, error))
            roots.push_back({{"name", path}, {"path", path}});
    }
#else
    roots.push_back({{"name", "/"}, {"path", "/"}});
    for (const char* place : {"/mnt", "/media"}) {
        std::error_code error;
        if (fs::is_directory(place, error))
            roots.push_back({{"name", place}, {"path", place}});
    }
#endif
    roots.insert(roots.begin(), json{{"name", "Home"}, {"path", home_folder().u8string()}});
    return roots;
}

// The folder of the running sd-server executable.
fs::path executable_folder() {
    std::error_code error;
#ifdef _WIN32
    wchar_t buffer[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length > 0 && length < MAX_PATH)
        return fs::path(buffer).parent_path();
#else
    const fs::path self = fs::read_symlink("/proc/self/exe", error);
    if (!error)
        return self.parent_path();
#endif
    return fs::current_path(error);
}

}  // namespace

void apply_studio_settings(SDSvrParams& params) {
    params.folders_configured = false;
    if (params.settings_file == "none") {
        params.settings_file.clear();
        return;
    }
    // Started with no folder options (for example by double-clicking sd-server), keep the
    // folders next to the executable so the Web UI can ask for them and save them.
    if (params.settings_file.empty() && params.model_dirs.empty())
        params.settings_file = (executable_folder() / "studio-settings.json").u8string();
    if (params.settings_file.empty())
        return;
    const json settings = read_settings(params.settings_file);
    int ready           = 0;
    for (const char* key : k_folder_keys) {
        const std::string path = folder_setting(settings, key);
        if (!is_directory(path)) {
            if (!path.empty())
                LOG_WARN("the %s folder in %s does not exist: %s", key, params.settings_file.c_str(), path.c_str());
            continue;
        }
        ++ready;
        if (std::string(key) == "models")
            params.model_dirs = {path};
        else if (std::string(key) == "inputs")
            params.input_dir = path;
        else
            params.output_dir = path;
    }
    params.folders_configured = ready == 3;
    if (const auto url = settings.find("catalog_url"); url != settings.end() && url->is_string())
        params.catalog_url = url->get<std::string>() == "none" ? "" : url->get<std::string>();
}

void register_studio_settings_endpoints(httplib::Server& svr, ServerRuntime& runtime) {
    ServerRuntime* rt = &runtime;

    svr.Get("/sdcpp/v1/settings/folders", [rt](const httplib::Request& req, httplib::Response& res) {
        const SDSvrParams& params = *rt->svr_params;
        const fs::path base       = base_folder(params);
        json folders              = json::object();
        json suggested            = json::object();
        for (const char* key : k_folder_keys) {
            const std::string path = absolute_path(current_folder(params, key));
            folders[key]           = {{"path", path}, {"exists", is_directory(path)}};
            suggested[key]         = (base / key).u8string();
        }
        reply(res, 200, {{"settings_file", params.settings_file.empty() ? "" : absolute_path(params.settings_file)},
                         {"can_save", !params.settings_file.empty()},
                         {"needs_setup", !params.settings_file.empty() && !params.folders_configured},
                         {"can_browse", from_this_computer(req)},
                         {"folders", folders},
                         {"suggested", suggested}});
    });

    // Saves the three folders (creating missing ones) and switches to them without a restart.
    svr.Put("/sdcpp/v1/settings/folders", [rt](const httplib::Request& req, httplib::Response& res) {
        // svr_params is shared read-only while serving; folder changes are the one runtime update.
        auto& params = const_cast<SDSvrParams&>(*rt->svr_params);
        if (params.settings_file.empty())
            return reply_error(res, 400, "the server was started without --settings-file, so folders cannot be saved");
        const json body = json::parse(req.body, nullptr, false);
        if (!body.is_object())
            return reply_error(res, 400, "expected a JSON object");

        json chosen = json::object();
        for (const char* key : k_folder_keys) {
            const std::string path = body.value(key, std::string());
            if (path.empty())
                return reply_error(res, 400, std::string("choose a folder for ") + key);
            const fs::path folder = fs::u8path(path);
            if (!folder.is_absolute())
                return reply_error(res, 400, std::string("the ") + key + " folder must be a full path, like " + (base_folder(params) / key).u8string());
            std::error_code error;
            fs::create_directories(folder, error);
            if (error || !fs::is_directory(folder, error))
                return reply_error(res, 400, std::string("cannot use the ") + key + " folder " + path + ": " + (error ? error.message() : "not a folder"));
            chosen[std::string(key) + "_dir"] = folder.lexically_normal().u8string();
        }
        {
            std::lock_guard<std::mutex> lock(rt->async_job_manager->mutex);
            if (count_pending_jobs(*rt->async_job_manager) > 0)
                return reply_error(res, 409, "wait until the queued and running jobs finish, then save the folders again");
        }

        json settings = read_settings(params.settings_file);
        settings.update(chosen);
        {
            std::ofstream out(fs::u8path(params.settings_file), std::ios::binary | std::ios::trunc);
            out << settings.dump(2) << "\n";
            if (!out)
                return reply_error(res, 500, "cannot write " + params.settings_file);
        }
        try {
            // Holding the generation lock keeps jobs from reading the folders while they change.
            std::lock_guard<std::mutex> lock(*rt->sd_ctx_mutex);
            params.model_dirs         = {chosen["models_dir"].get<std::string>()};
            params.input_dir          = chosen["inputs_dir"].get<std::string>();
            params.output_dir         = chosen["outputs_dir"].get<std::string>();
            params.folders_configured = true;
            configure_server_file_access(*rt->models, params, *rt->ctx_params);
        } catch (const std::exception& error) {
            return reply_error(res, 500, std::string("the folders were saved but could not be applied; restart the server: ") + error.what());
        }
        refresh_model_catalog(*rt);
        LOG_INFO("folders changed: models %s, inputs %s, outputs %s", params.model_dirs.front().c_str(), params.input_dir.c_str(),
                 params.output_dir.c_str());
        reply(res, 200, {{"saved", true}});
    });

    // Where the Models page's online list comes from; saved so it survives restarts.
    svr.Get("/sdcpp/v1/settings/catalog", [rt](const httplib::Request&, httplib::Response& res) {
        reply(res, 200, {{"url", rt->svr_params->catalog_url}, {"can_save", !rt->svr_params->settings_file.empty()}});
    });

    svr.Put("/sdcpp/v1/settings/catalog", [rt](const httplib::Request& req, httplib::Response& res) {
        auto& params = const_cast<SDSvrParams&>(*rt->svr_params);
        if (params.settings_file.empty())
            return reply_error(res, 400, "the server was started without --settings-file, so the catalog source cannot be saved");
        const json body        = json::parse(req.body, nullptr, false);
        const std::string url  = body.is_object() ? body.value("url", std::string()) : "";
        if (!url.empty() && url.rfind("https://", 0) != 0)
            return reply_error(res, 400, "the catalog address must start with https://");
        json settings          = read_settings(params.settings_file);
        settings["catalog_url"] = url.empty() ? "none" : url;
        std::ofstream out(fs::u8path(params.settings_file), std::ios::binary | std::ios::trunc);
        out << settings.dump(2) << "\n";
        if (!out)
            return reply_error(res, 500, "cannot write " + params.settings_file);
        params.catalog_url = url;
        LOG_INFO("catalog source: %s", url.empty() ? "none" : url.c_str());
        reply(res, 200, {{"url", url}});
    });

    svr.Get("/sdcpp/v1/fs/dirs", [](const httplib::Request& req, httplib::Response& res) {
        if (!from_this_computer(req))
            return reply_error(res, 403, "folders can only be browsed on the computer that runs the server");
        std::error_code error;
        fs::path folder = req.has_param("path") && !req.get_param_value("path").empty() ? fs::u8path(req.get_param_value("path")) : home_folder();
        folder          = fs::absolute(folder, error).lexically_normal();
        if (!fs::is_directory(folder, error))
            return reply_error(res, 404, "not a folder: " + folder.u8string());
        json dirs = json::array();
        std::string warning;
        for (fs::directory_iterator it(folder, fs::directory_options::skip_permission_denied, error), end; !error && it != end; it.increment(error)) {
            std::error_code entry_error;
            if (hidden_folder(it->path()) || !it->is_directory(entry_error))
                continue;
            dirs.push_back({{"name", it->path().filename().u8string()}, {"path", it->path().u8string()}});
        }
        if (error)
            warning = "this folder cannot be read (" + error.message() + ")";
        std::sort(dirs.begin(), dirs.end(), [](const json& a, const json& b) { return a["name"].get<std::string>() < b["name"].get<std::string>(); });
        const fs::path parent = folder.parent_path();
        reply(res, 200, {{"path", folder.u8string()},
                         {"parent", parent == folder ? "" : parent.u8string()},
                         {"dirs", dirs},
                         {"roots", folder_roots()},
                         {"warning", warning}});
    });

    svr.Post("/sdcpp/v1/fs/mkdir", [](const httplib::Request& req, httplib::Response& res) {
        if (!from_this_computer(req))
            return reply_error(res, 403, "folders can only be created on the computer that runs the server");
        const json body        = json::parse(req.body, nullptr, false);
        const std::string path = body.is_object() ? body.value("path", std::string()) : "";
        if (path.empty() || !fs::u8path(path).is_absolute())
            return reply_error(res, 400, "expected the full path of the new folder");
        std::error_code error;
        fs::create_directories(fs::u8path(path), error);
        if (error)
            return reply_error(res, 400, "cannot create " + path + ": " + error.message());
        reply(res, 200, {{"path", fs::u8path(path).lexically_normal().u8string()}});
    });
}

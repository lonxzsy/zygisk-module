#include <cerrno>
#include <fcntl.h>
#include <jni.h>
#include <unistd.h>

#include <cstring>
#include <string>

#include "config.h"
#include "il2cpp_dumper.h"
#include "overlay.h"
#include "zygisk.hpp"

using zygisk::Api;
using zygisk::AppSpecializeArgs;
using zygisk::ServerSpecializeArgs;

static bool read_all(int fd, void *buf, size_t n) {
    auto *p = static_cast<char *>(buf);
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (r == 0) return false;
        p += r;
        n -= static_cast<size_t>(r);
    }
    return true;
}

static void write_all(int fd, const void *buf, size_t n) {
    auto *p = static_cast<const char *>(buf);
    while (n) {
        ssize_t r = write(fd, p, n);
        if (r < 0) {
            if (errno == EINTR) continue;
            return;
        }
        if (r == 0) return;
        p += r;
        n -= static_cast<size_t>(r);
    }
}

class DumperModule : public zygisk::ModuleBase {
public:
    void onLoad(Api *api, JNIEnv *env) override {
        api_ = api;
        env_ = env;
    }

    void preAppSpecialize(AppSpecializeArgs *args) override {
        if (!args || !args->nice_name) {
            api_->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
            return;
        }
        const char *name = env_->GetStringUTFChars(args->nice_name, nullptr);
        std::string proc = name ? name : "";
        if (name) env_->ReleaseStringUTFChars(args->nice_name, name);

        if (proc.empty()) {
            api_->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        // Ask the root companion for the config text (app process cannot read /data/adb).
        std::string text;
        int fd = api_->connectCompanion();
        if (fd >= 0) {
            uint32_t len = 0;
            if (read_all(fd, &len, sizeof len) && len < (1u << 20)) {
                text.resize(len);
                if (len && !read_all(fd, text.data(), len)) text.clear();
            }
            close(fd);
        }
        cfg_ = parse_config(text);
        if (cfg_.matches(proc)) {
            target_ = true;
            // Base package name without ":subprocess" suffix for Android/data/<pkg> paths
            auto colon = proc.find(':');
            pkg_ = (colon != std::string::npos) ? proc.substr(0, colon) : proc;
        } else {
            api_->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
        }
    }

    void postAppSpecialize(const AppSpecializeArgs *) override {
        if (!target_) return;
        dumper::start(cfg_, pkg_);
        if (cfg_.overlay) {
            overlay::install(env_);
        }
    }

    void preServerSpecialize(ServerSpecializeArgs *) override {
        api_->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
    }

private:
    Api *api_ = nullptr;
    JNIEnv *env_ = nullptr;
    Config cfg_;
    std::string pkg_;
    bool target_ = false;
};

static void companion_handler(int client) {
    std::string text;
    int fd = open(kConfigPath, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        char buf[4096];
        for (;;) {
            ssize_t r = read(fd, buf, sizeof buf);
            if (r < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (r == 0) break;
            text.append(buf, static_cast<size_t>(r));
        }
        close(fd);
    }
    uint32_t len = static_cast<uint32_t>(text.size());
    write_all(client, &len, sizeof len);
    if (len > 0) {
        write_all(client, text.data(), len);
    }
}

REGISTER_ZYGISK_MODULE(DumperModule)
REGISTER_ZYGISK_COMPANION(companion_handler)

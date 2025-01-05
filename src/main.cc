
#include <filesystem>
#include <iostream>
#include <thread>
#include <vector>

#define _SILENCE_EXPERIMENTAL_FILESYSTEM_DEPRECATION_WARNING
#pragma warning(disable: 4996)
#include "node.h"
#include "uv.h"
#include "v8-cppgc.h"
#include "v8.h"

using string = std::string;
namespace fs = std::filesystem;

// libnpm.exe /path/to/node_modules
int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <path/to/node_modules>\n";
        return 1;
    }

    string node_modules = argv[1]; // path to node_modules (absolute)
    if (!fs::exists(node_modules)) {
        std::cerr << "Error: " << node_modules << " does not exist\n";
        return 1;
    }

    fs::path npm = fs::path(node_modules) / "npm";
    if (!fs::exists(npm)) {
        std::cerr << "Error: " << npm << " does not exist\n";
        return 1;
    }

    std::cout << "Found npm at " << npm << "\n";

    // initialize libnode
    auto                workingDir = fs::current_path() / "libnpm.exe";
    std::vector<string> args       = {workingDir.string()};
    std::vector<string> execArgs   = {};

    char* cWorkingDir = const_cast<char*>(workingDir.string().c_str());
    uv_setup_args(1, &cWorkingDir);
    cppgc::InitializeProcess();
    std::vector<string> errors;
    if (node::InitializeNodeWithArgs(&args, &execArgs, &errors) != 0) {
        std::cerr << "Failed to initialize Node.js: ";
        for (auto const& error : errors) {
            std::cerr << error << "\n";
        }
        return 1;
    }
    auto platForm = node::MultiIsolatePlatform::Create(std::thread::hardware_concurrency());
    v8::V8::InitializePlatform(platForm.get());
    v8::V8::Initialize();


    // run npm
    std::unique_ptr<node::CommonEnvironmentSetup> setup = node::CommonEnvironmentSetup::Create(
        platForm.get(),
        &errors,
        args,
        execArgs,
        node::EnvironmentFlags::kOwnsProcessState
    );
    if (!setup) {
        for (const std::string& err : errors) std::cerr << err << "\n";
        return 1;
    }

    auto               executeDir = fs::current_path();
    v8::Isolate*       isolate    = setup->isolate();
    node::Environment* env        = setup->env();

    v8::Locker         locker(isolate);
    v8::Isolate::Scope isolate_scope(isolate);
    v8::HandleScope    handle_scope(isolate);
    v8::Context::Scope context_scope(setup->context());

    // clang-format off
    string compiler = R"(
        try {
            const path = require("path");
            const process = require("process");

            globalThis.require = require('module').createRequire(path.join(")"+fs::path(node_modules).parent_path().string()+R"("));

            const code = `
                (async function () {
                    const path = require("path");
                    const process = require("process");
                    try {
                        const NPM = require(path.join(
                            ")"+node_modules+R"(",
                            "npm/lib/npm.js"
                        ));
                        const npm = new NPM();
                        await npm.load();
                        await npm.exec("install", []);

                        console.info("npm install finished");
                        process.exit(0);
                    } catch (error) {
                        console.error(error);
                        process.exit(1);
                    }
                })();
            `;
            require("vm").runInThisContext(code, "libnpm-install.js");
        } catch (e) {
            console.error(`Failed to run npm install:\n${e.stack}`);
        }
    )";
    // clang-format on

    int code = 0;
    try {
        node::SetProcessExitHandler(env, [&](node::Environment* env_, int exit_code) { node::Stop(env); });
        v8::MaybeLocal<v8::Value> loadValue = node::LoadEnvironment(env, compiler.c_str());
        if (loadValue.IsEmpty()) {
            std::cerr << "Failed to load environment\n";
            return 1;
        }
        code = node::SpinEventLoop(env).FromMaybe(1);
    } catch (...) {
        return 1;
    }

    node::Stop(env);
    v8::V8::Dispose();
    v8::V8::DisposePlatform();
    return code;
}
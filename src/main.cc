
#include "fmt/format.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>

#define _SILENCE_EXPERIMENTAL_FILESYSTEM_DEPRECATION_WARNING
#pragma warning(disable : 4996)
#include "node.h"
#include "uv.h"
#include "v8-cppgc.h"
#include "v8.h"

using string = std::string;
namespace fs = std::filesystem;


// libnpm.exe [--npm_dir] <npm args...>
// 自定义参数 --npm_dir 指定 npm 的路径
// 例如: libnpm.exe --npm_dir=/path/to/node_modules/npm install
// 不指定 --npm_dir 时, 默认使用当前目录下的 node_modules/npm
int main(int argc, char* argv[]) {
    std::vector<string> args(argv, argv + argc); // 拷贝参数
    if (args.size() < 2) {
        fmt::print("Usage: {} [--npm_dir] <npm args...>\n", args[0]);
        return 1;
    }

    // 检查是否有 --npm_dir 参数
    fs::path npm_dir = fs::current_path() / "node_modules" / "npm";
    for (int i = 1; i < args.size(); i++) {
        if (args[i] == "--npm_dir") {
            if (i + 1 >= args.size()) {
                fmt::print("Error: --npm_dir requires a path\n");
                return 1;
            }
            // 检查路径是否存在
            npm_dir = args[i + 1];
            if (!fs::exists(npm_dir)) {
                fmt::print("Error: {} does not exist\n", npm_dir.string());
                return 1;
            }
            args.erase(args.begin() + i, args.begin() + i + 2); // 删除 --npm_dir 和其后的路径
            break;
        }
    }
    // 检查 npm 是否存在
    if (!fs::exists(npm_dir)) {
        fmt::print("Error: {} does not exist\n", npm_dir.string());
        return 1;
    }

    // 初始化 libnode
    auto                _working_process   = fs::current_path() / "libnpm.exe";
    std::vector<string> exec_argv          = {};
    char*               _c_working_process = const_cast<char*>(_working_process.string().c_str());
    uv_setup_args(1, &_c_working_process);
    cppgc::InitializeProcess();
    std::vector<string> errors;
    if (node::InitializeNodeWithArgs(&args, &exec_argv, &errors) != 0) {
        std::cerr << "Failed to initialize Node.js: ";
        for (auto const& error : errors) {
            std::cerr << error << "\n";
        }
        return 1;
    }
    auto platform = node::MultiIsolatePlatform::Create(std::thread::hardware_concurrency());
    v8::V8::InitializePlatform(platform.get());
    v8::V8::Initialize();

    // 创建环境
    std::unique_ptr<node::CommonEnvironmentSetup> setup = node::CommonEnvironmentSetup::Create(
        platform.get(),
        &errors,
        args,
        exec_argv,
        node::EnvironmentFlags::kOwnsProcessState
    );
    if (!setup) {
        for (const std::string& err : errors) std::cerr << err << "\n";
        return 1;
    }

    v8::Isolate*       isolate = setup->isolate();
    node::Environment* env     = setup->env();

    v8::Locker         locker(isolate);
    v8::Isolate::Scope isolate_scope(isolate);
    v8::HandleScope    handle_scope(isolate);

    auto               context = setup->context();
    v8::Context::Scope context_scope(context);

    // clang-format off
    string compiler = R"(
        try {
            const path = require("path");

            const npm_dir = path.join(")"+npm_dir.string()+R"(");
            const npm_dir_node_modules = path.join(npm_dir, "node_modules");

            const Module = require('module').Module;
            Module._resolveLookupPaths = function (request, parent) {
                result = [parent.path, npm_dir, npm_dir_node_modules];
                return result;
            };
            globalThis.require = Module.createRequire(npm_dir);

            require("vm").runInThisContext(` require('lib/cli.js')(process); `, "libnpm.exe");
        } catch (e) {
            console.error(e);
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

    node::SpinEventLoop(env).FromMaybe(false); // 运行事件循环

    // 清理资源
    node::Stop(env);
    v8::V8::Dispose();
    v8::V8::DisposePlatform();
    return code;
}
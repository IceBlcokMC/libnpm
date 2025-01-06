
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

    fs::path npm_cli = npm_dir / "lib" / "cli.js";
    if (!fs::exists(npm_cli)) {
        fmt::print("Error: {} does not exist\n", npm_cli.string());
        return 1;
    }

    std::ifstream ifs(npm_cli);
    if (!ifs.is_open()) {
        fmt::print("Error: Failed to open {}\n", npm_cli.string());
        return 1;
    }

    // 读取 npm-cli.js 文件内容
    string npm_cli_content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    ifs.close();

    // 执行 npm-cli.js
    v8::Local<v8::String> source =
        v8::String::NewFromUtf8(isolate, npm_cli_content.c_str(), v8::NewStringType::kNormal).ToLocalChecked();

    // 编译源码
    v8::Local<v8::Script> script;
    if (!v8::Script::Compile(setup->context(), source).ToLocal(&script)) {
        fmt::print("Error: Failed to compile npm-cli.js\n");
        return 1;
    }

    // 运行脚本
    v8::Local<v8::Value> result;
    if (!script->Run(setup->context()).ToLocal(&result)) {
        fmt::print("Error: Failed to run npm-cli.js\n");
        return 1;
    }

    node::SpinEventLoop(env).FromMaybe(false); // 运行事件循环

    // 清理资源
    setup.reset();
    v8::V8::Dispose();
    v8::V8::DisposePlatform();
    return 0;
}
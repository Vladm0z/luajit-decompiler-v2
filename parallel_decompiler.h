// parallel_decompiler.h
#pragma once
#include <future>
#include <thread>
#include <vector>
#include <string>
#include <iostream>
#include <algorithm>
#include <mutex>
#include <atomic>

struct Error; 

struct DecompilerConfig {
    bool forceOverwrite;
    bool ignoreDebugInfo;
    bool minimizeDiffs;
    bool unrestrictedAscii;
};

extern std::mutex g_print_mutex;
extern std::atomic<uint32_t> g_filesProcessed;
extern std::atomic<uint32_t> g_filesFailed;
extern void print(const std::string& message);

void decompile_file_safe(const std::string& inputPath, const std::string& outputPath, const DecompilerConfig& config) {
    try {
        Bytecode bytecode(inputPath);
        bytecode();
        Ast ast(bytecode, config.ignoreDebugInfo, config.minimizeDiffs);
        ast();
        Lua lua(bytecode, ast, outputPath, config.forceOverwrite, config.minimizeDiffs, config.unrestrictedAscii);
        lua();
        
        g_filesProcessed++;
    } catch (const Error& error) {
        g_filesFailed++;
        std::lock_guard<std::mutex> lock(g_print_mutex);
        std::cerr << "[Error] " << inputPath << "\n"
                  << "  Source: " << error.source << ":" << error.line << "\n"
                  << "  " << error.message << "\n";
    } catch (const std::exception& e) {
        g_filesFailed++;
        std::lock_guard<std::mutex> lock(g_print_mutex);
        std::cerr << "[Error] " << inputPath << ": " << e.what() << "\n";
    } catch (...) {
        g_filesFailed++;
        std::lock_guard<std::mutex> lock(g_print_mutex);
        std::cerr << "[Error] " << inputPath << ": Unknown exception/Assertion\n";
    }
}

void decompile_all_parallel(const std::vector<std::string>& inputPaths, const std::vector<std::string>& outputPaths, const DecompilerConfig& config) {
    unsigned int num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 4;

    {
        std::lock_guard<std::mutex> lock(g_print_mutex);
        print("Starting parallel decompilation across " + std::to_string(num_threads) + " threads...");
    }

    std::vector<std::future<void>> futures;
    size_t chunk_size = (inputPaths.size() + num_threads - 1) / num_threads;

    for (unsigned int t = 0; t < num_threads; ++t) {
        size_t start = t * chunk_size;
        size_t end = (std::min)(start + chunk_size, inputPaths.size());
        if (start >= inputPaths.size()) break;

        futures.push_back(std::async(std::launch::async, [&inputPaths, &outputPaths, &config, start, end]() {
            for (size_t i = start; i < end; ++i) {
                decompile_file_safe(inputPaths[i], outputPaths[i], config);
            }
        }));
    }

    for (auto& f : futures) f.get();
    
    {
        std::lock_guard<std::mutex> lock(g_print_mutex);
        print("Parallel decompilation complete. Success: " + std::to_string(g_filesProcessed.load()) + ", Failed: " + std::to_string(g_filesFailed.load()));
    }
}
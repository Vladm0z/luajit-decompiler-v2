#pragma once

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <limits>
#include <chrono>

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
extern std::atomic<uint64_t> g_nsBytecode;
extern std::atomic<uint64_t> g_nsAst;
extern std::atomic<uint64_t> g_nsLua;
extern std::atomic<uint64_t> g_nsTotal;
extern std::atomic<uint32_t> g_slowFiles;
extern void print(const std::string& message);

namespace {

inline unsigned int resolve_thread_count(size_t fileCount) {
    unsigned int hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 1;

    char* env = nullptr;
    size_t envSize = 0;

    errno_t err = _dupenv_s(&env, &envSize, "LJD_MAX_THREADS");

    if (err == 0 && env != nullptr) {
        char* end = nullptr;
        unsigned long parsed = std::strtoul(env, &end, 10);

        if (
            end != env &&
            parsed > 0 &&
            parsed <= static_cast<unsigned long>((std::numeric_limits<unsigned int>::max)())
        ) {
            hw = static_cast<unsigned int>(parsed);
        }

        std::free(env);
    }

    size_t threadCount = static_cast<size_t>(hw);

    if (threadCount > fileCount) {
        threadCount = fileCount;
    }

    if (threadCount == 0) {
        threadCount = 1;
    }

    return static_cast<unsigned int>(threadCount);
}

inline size_t resolve_batch_size(size_t fileCount, unsigned int threadCount) {
    size_t targetBatches = static_cast<size_t>(threadCount) * 6;

    if (targetBatches == 0) {
        targetBatches = 1;
    }

    size_t batch = (fileCount + targetBatches - 1) / targetBatches;

    if (batch < 1) {
        batch = 1;
    }

    if (batch > 128) {
        batch = 128;
    }

    return batch;
}

}

inline bool looks_like_luajit_bytecode(const std::string& path) {
    HANDLE file = CreateFileA(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    uint8_t buffer[3];
    DWORD bytesRead = 0;

    BOOL ok = ReadFile(
        file,
        buffer,
        sizeof(buffer),
        &bytesRead,
        NULL
    );

    CloseHandle(file);

    if (!ok || bytesRead < 3) {
        return false;
    }

    return buffer[0] == 0x1B && (
        (buffer[1] == 'L' && buffer[2] == 'J') ||
        (buffer[1] == 'F' && buffer[2] == 'S')
    );
}

namespace detail {

thread_local uint64_t tl_nsBytecode = 0;
thread_local uint64_t tl_nsAst = 0;
thread_local uint64_t tl_nsLua = 0;
thread_local uint64_t tl_nsTotal = 0;

inline int decompile_seh_filter(unsigned int code) {
    constexpr unsigned int MSVC_CPP_EXCEPTION = 0xE06D7363u;

    return code == MSVC_CPP_EXCEPTION
        ? EXCEPTION_CONTINUE_SEARCH
        : EXCEPTION_EXECUTE_HANDLER;
}

inline void log_seh_failure(const std::string& inputPath, unsigned int code) {
    std::lock_guard<std::mutex> lock(g_print_mutex);
    std::cerr
        << "[SEH] "
        << inputPath
        << ": exception code 0x"
        << std::hex << code << std::dec
        << "\n";
}

inline void decompile_file_core(
    const std::string& inputPath,
    const std::string& outputPath,
    const DecompilerConfig& config
) {
    using Clock = std::chrono::high_resolution_clock;

    const auto t0 = Clock::now();

    Bytecode bytecode(inputPath);
    bytecode();

    const auto t1 = Clock::now();

    Ast ast(bytecode, config.ignoreDebugInfo, config.minimizeDiffs);
    ast();

    const auto t2 = Clock::now();

    Lua lua(
        bytecode,
        ast,
        outputPath,
        config.forceOverwrite,
        config.minimizeDiffs,
        config.unrestrictedAscii
    );
    lua();

    const auto t3 = Clock::now();

	uint64_t nsBc = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
	uint64_t nsAst = std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count();
	uint64_t nsLua = std::chrono::duration_cast<std::chrono::nanoseconds>(t3 - t2).count();
	uint64_t nsTotal = std::chrono::duration_cast<std::chrono::nanoseconds>(t3 - t0).count();

	// Accumulate into thread-local storage
	tl_nsBytecode += nsBc;
	tl_nsAst += nsAst;
	tl_nsLua += nsLua;
	tl_nsTotal += nsTotal;

    if (std::chrono::duration_cast<std::chrono::milliseconds>(t3 - t0).count() > 5000) {
        g_slowFiles.fetch_add(1, std::memory_order_relaxed);
    }
}

inline bool decompile_file_seh(
    const std::string& inputPath,
    const std::string& outputPath,
    const DecompilerConfig& config
) {
    __try {
        detail::decompile_file_core(inputPath, outputPath, config);
        return true;
    }
    __except (detail::decompile_seh_filter(GetExceptionCode())) {
        const unsigned int code = GetExceptionCode();
        detail::log_seh_failure(inputPath, code);
        return false;
    }
}

} // namespace detail

inline void decompile_file_safe(
    const std::string & inputPath,
    const std::string & outputPath,
    const DecompilerConfig & config
) {
    try {
        if (!detail::decompile_file_seh(inputPath, outputPath, config)) {
            g_filesFailed.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        g_filesProcessed.fetch_add(1, std::memory_order_relaxed);
    }
    catch (const Error& error) {
        g_filesFailed.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(g_print_mutex);
        std::cerr
            << "[Error] " << inputPath << "\n"
            << "  Source: " << error.source << ":" << error.line << "\n"
            << "  " << error.message << "\n";
    }
    catch (const std::exception& e) {
        g_filesFailed.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(g_print_mutex);
        std::cerr << "[Error] " << inputPath << ": " << e.what() << "\n";
    }
    catch (...) {
        g_filesFailed.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(g_print_mutex);
        std::cerr << "[Error] " << inputPath << ": Unknown exception/Assertion\n";
    }
}

inline void decompile_all_parallel(
    const std::vector<std::string>& inputPaths,
    const std::vector<std::string>& outputPaths,
    const DecompilerConfig& config
) {
    const size_t total = inputPaths.size();

    if (total == 0) {
        return;
    }

    if (total == 1) {
        decompile_file_safe(inputPaths[0], outputPaths[0], config);
        
        g_nsBytecode.fetch_add(detail::tl_nsBytecode, std::memory_order_relaxed);
		g_nsAst.fetch_add(detail::tl_nsAst, std::memory_order_relaxed);
		g_nsLua.fetch_add(detail::tl_nsLua, std::memory_order_relaxed);
		g_nsTotal.fetch_add(detail::tl_nsTotal, std::memory_order_relaxed);

        std::lock_guard<std::mutex> lock(g_print_mutex);
        print(
            "Decompilation complete. Success: " +
            std::to_string(g_filesProcessed.load()) +
            ", Failed: " +
            std::to_string(g_filesFailed.load())
        );
        print(
            "Phase time totals -> Bytecode: " +
            std::to_string(g_nsBytecode.load() / 1000000000ull) +
            " s, AST: " +
            std::to_string(g_nsAst.load() / 1000000000ull) +
            " s, Lua: " +
            std::to_string(g_nsLua.load() / 1000000000ull) +
            " s, Total: " +
            std::to_string(g_nsTotal.load() / 1000000000ull) +
            " s"
        );

        return;
    }

    const unsigned int threadCount = resolve_thread_count(total);
    const size_t batchSize = resolve_batch_size(total, threadCount);

    {
        std::lock_guard<std::mutex> lock(g_print_mutex);
        print(
            "Starting parallel decompilation across " +
            std::to_string(threadCount) +
            " threads, dynamic batch size " +
            std::to_string(batchSize) +
            "..."
        );
    }

    std::atomic<size_t> nextIndex{0};

    std::vector<std::thread> workers;
    workers.reserve(threadCount);

    for (unsigned int t = 0; t < threadCount; ++t) {
        workers.emplace_back(
            [&inputPaths, &outputPaths, &config, &nextIndex, total, batchSize]() {
                while (true) {
                    const size_t start = nextIndex.fetch_add(
                        batchSize,
                        std::memory_order_relaxed
                    );

                    if (start >= total) {
                        break;
                    }

                    size_t end = start + batchSize;

                    if (end > total) {
                        end = total;
                    }

                    for (size_t i = start; i < end; ++i) {
                        decompile_file_safe(
                            inputPaths[i],
                            outputPaths[i],
                            config
                        );
                    }
                }
                
                // Merge thread-local timings into globals ONCE per worker thread
                g_nsBytecode += detail::tl_nsBytecode;
                g_nsAst += detail::tl_nsAst;
                g_nsLua += detail::tl_nsLua;
                g_nsTotal += detail::tl_nsTotal;
            }
        );
    }

    for (std::thread& worker : workers) {
        worker.join();
    }

    {
        std::lock_guard<std::mutex> lock(g_print_mutex);
        print(
            "Parallel decompilation complete. Success: " +
            std::to_string(g_filesProcessed.load(std::memory_order_relaxed)) +
            ", Failed: " +
            std::to_string(g_filesFailed.load(std::memory_order_relaxed))
        );
        print(
            "Phase time totals -> Bytecode: " +
            std::to_string(g_nsBytecode.load() / 1000000000ull) +
            " s, AST: " +
            std::to_string(g_nsAst.load() / 1000000000ull) +
            " s, Lua: " +
            std::to_string(g_nsLua.load() / 1000000000ull) +
            " s, Total: " +
            std::to_string(g_nsTotal.load() / 1000000000ull) +
            " s"
        );
    }
}
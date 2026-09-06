#include "udf_writer.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#  include <process.h>
#  include <windows.h>
#else
#  include <pthread.h>
#endif

namespace fs = std::filesystem;

struct Context {
    fs::path source;
    fs::path output;
    std::string error;
};

static void run_writer(Context& context) {
    try {
        udf25::WriterOptions options;
        options.output = context.output;
        options.volume_label = "STACKTEST";
        options.application_id = "*BDMVAuthor";
        options.implementation_id = "*BDMVAuthor";
        options.automatic_bdmv_correction = false;
        options.automatic_hddvd_correction = false;
        options.verify_after_write = true;
        options.grafts.push_back({context.source, ""});
        udf25::ImageWriter writer(std::move(options));
        (void)writer.build();
    } catch (const std::exception& e) {
        context.error = e.what();
    } catch (...) {
        context.error = "unknown exception";
    }
}

#ifdef _WIN32
static unsigned __stdcall writer_thread(void* opaque) {
    run_writer(*static_cast<Context*>(opaque));
    return 0;
}
#else
static void* writer_thread(void* opaque) {
    run_writer(*static_cast<Context*>(opaque));
    return nullptr;
}
#endif

int main() {
    const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const fs::path work = fs::temp_directory_path() /
                          ("bdmvauthor-udf-small-stack-" + std::to_string(stamp));
    const fs::path source = work / "source";
    const fs::path output = work / "test.iso";

    try {
        fs::create_directories(source);
        std::ofstream payload(source / "payload.bin", std::ios::binary | std::ios::trunc);
        if (!payload) throw std::runtime_error("cannot create payload fixture");
        std::vector<char> block(64 * 1024, static_cast<char>(0x5a));
        for (int i = 0; i < 32; ++i) payload.write(block.data(), static_cast<std::streamsize>(block.size()));
        payload.close();
        if (!payload) throw std::runtime_error("cannot finish payload fixture");

        Context context{source, output, {}};
        constexpr std::size_t kSmallStack = 512 * 1024;
#ifdef _WIN32
        const uintptr_t handle = _beginthreadex(nullptr, static_cast<unsigned>(kSmallStack),
                                                writer_thread, &context, 0, nullptr);
        if (handle == 0) throw std::runtime_error("_beginthreadex failed");
        const DWORD wait = WaitForSingleObject(reinterpret_cast<HANDLE>(handle), INFINITE);
        CloseHandle(reinterpret_cast<HANDLE>(handle));
        if (wait != WAIT_OBJECT_0) throw std::runtime_error("waiting for writer thread failed");
#else
        pthread_attr_t attr;
        if (pthread_attr_init(&attr) != 0) throw std::runtime_error("pthread_attr_init failed");
        const int stack_result = pthread_attr_setstacksize(&attr, kSmallStack);
        if (stack_result != 0) {
            pthread_attr_destroy(&attr);
            throw std::runtime_error("pthread_attr_setstacksize failed");
        }
        pthread_t thread{};
        const int create_result = pthread_create(&thread, &attr, writer_thread, &context);
        pthread_attr_destroy(&attr);
        if (create_result != 0) throw std::runtime_error("pthread_create failed");
        if (pthread_join(thread, nullptr) != 0) throw std::runtime_error("pthread_join failed");
#endif
        if (!context.error.empty()) throw std::runtime_error("writer failed: " + context.error);
        if (!fs::exists(output) || fs::file_size(output) <= 2 * 1024 * 1024)
            throw std::runtime_error("writer did not produce the expected image");

        bool cancellation_seen = false;
        try {
            udf25::WriterOptions cancelled;
            cancelled.output = work / "cancelled.iso";
            cancelled.volume_label = "CANCELTEST";
            cancelled.application_id = "*BDMVAuthor";
            cancelled.implementation_id = "*BDMVAuthor";
            cancelled.automatic_bdmv_correction = false;
            cancelled.automatic_hddvd_correction = false;
            cancelled.cancel_requested = [] { return true; };
            cancelled.grafts.push_back({source, ""});
            udf25::ImageWriter cancelled_writer(std::move(cancelled));
            (void)cancelled_writer.build();
        } catch (const std::exception& e) {
            cancellation_seen = std::string(e.what()).find("cancelled") != std::string::npos;
        }
        if (!cancellation_seen) throw std::runtime_error("UDF cancellation callback did not stop image creation");

        std::error_code ec;
        fs::remove_all(work, ec);
        std::cout << "UDF small-stack streaming ok\n";
        return 0;
    } catch (const std::exception& e) {
        std::error_code ec;
        fs::remove_all(work, ec);
        std::cerr << e.what() << '\n';
        return 1;
    }
}

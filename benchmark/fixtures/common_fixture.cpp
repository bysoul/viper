#include "common_fixture.hpp"
#include <fcntl.h>
#include <sys/mman.h>
#include <thread>

namespace viper::kv_bm {

std::string random_file(const std::filesystem::path& base_dir) {
    if (!std::filesystem::exists(base_dir)) {
        if (!std::filesystem::create_directories(base_dir)) {
            throw std::runtime_error{"Could not create dir: " + base_dir.string() + "\n"};
        }
    }
    std::string str("abcdefghijklmnopqrstuvwxyz");
    std::random_device rd;
    std::mt19937 generator(rd());
    std::shuffle(str.begin(), str.end(), generator);
    std::string file_name = str.substr(0, 15) + ".file";
    std::filesystem::path file{file_name};
    return base_dir / file;
}

void BaseFixture::log_find_count(benchmark::State& state, uint64_t num_found, uint64_t num_expected) {
    state.counters["found"] = num_found;
    if (num_found != num_expected) {
        std::cerr << "DID NOT FIND ALL ENTRIES (" + std::to_string(num_found)
            + "/" + std::to_string(num_expected) + ")\n";
    }
}

void BaseFixture::prefill(const size_t num_prefills) {
    cpu_set_t cpuset_before;
    pthread_getaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset_before);
    set_cpu_affinity();

    std::vector<std::thread> prefill_threads{};
    const size_t num_prefills_per_thread = (num_prefills / NUM_UTIL_THREADS) + 1;

    for (size_t thread_num = 0; thread_num < NUM_UTIL_THREADS; ++thread_num) {
        const size_t start_key = thread_num * num_prefills_per_thread;
        const size_t end_key = std::min(start_key + num_prefills_per_thread, num_prefills);
        prefill_threads.emplace_back([&](const size_t start, const size_t end) {
            set_cpu_affinity(thread_num);
            this->insert(start, end);
        }, start_key, end_key);
    }

    for (std::thread& thread : prefill_threads) {
        thread.join();
    }

    set_cpu_affinity(CPU_ISSET(0, &cpuset_before) ? 0 : 1);
}

bool is_init_thread(const benchmark::State& state) {
    // Use idx = 1 because 0 starts all threads first before continuing.
    return state.threads == 1 || state.thread_index == 1;
}

void set_cpu_affinity() {
    const auto native_thread_handle = pthread_self();
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    for (int cpu : CPUS) {
        CPU_SET(cpu, &cpuset);
    }
    int rc = pthread_setaffinity_np(native_thread_handle, sizeof(cpu_set_t), &cpuset);
    if (rc != 0) {
        std::cerr << "Error calling pthread_setaffinity_np: " << rc << "\n";
    }
}

void set_cpu_affinity(uint16_t thread_idx) {
    if (thread_idx >= CPUS.size()) {
        throw std::runtime_error("Thread index too high!");
    }
    const auto native_thread_handle = pthread_self();
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(CPUS[thread_idx], &cpuset);
    int rc = pthread_setaffinity_np(native_thread_handle, sizeof(cpu_set_t), &cpuset);
    if (rc != 0) {
        std::cerr << "Error calling pthread_setaffinity_np: " << rc << "\n";
    }
}

void zero_block_device(const std::string& block_dev, size_t length) {
    int fd = open(block_dev.c_str(), O_RDWR);
    if (fd < 0) {
        throw std::runtime_error("Cannot open dax device: " + block_dev + " | " + std::strerror(errno));
    }

    void* addr = mmap(nullptr, length, PROT_WRITE, MAP_SHARED, fd, 0);
    if (addr == nullptr || addr == reinterpret_cast<void*>(0xffffffffffffffff)) {
        throw std::runtime_error("Cannot mmap pool file: " + block_dev + " | " + std::strerror(errno));
    }

    constexpr size_t buffer_size = 4096;
    std::array<char, buffer_size> buffer;
    buffer.fill(0);

    const size_t num_chunks = length / buffer_size;
    const size_t num_chunks_per_thread = (num_chunks / NUM_UTIL_THREADS) + 1;

    std::vector<std::thread> zero_threads;
    zero_threads.reserve(NUM_UTIL_THREADS);
    for (size_t thread_num = 0; thread_num < NUM_UTIL_THREADS; ++thread_num) {
        char* start_addr = reinterpret_cast<char*>(addr) + (thread_num * num_chunks_per_thread);
        zero_threads.emplace_back([&](char* start_addr) {
            for (size_t i = 0; i < num_chunks_per_thread; ++i) {
                void* chunk_start_addr = start_addr + (i * buffer_size);
                memcpy(chunk_start_addr, &buffer, buffer_size);
            }
        }, start_addr);
    }

    for (std::thread& thread : zero_threads) {
        thread.join();
    }

    munmap(addr, length);
}

}  // namespace viper::kv_bm
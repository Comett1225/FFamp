#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <random>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

namespace {

const size_t kEdges = 1 << 20;
uint8_t hits[kEdges];
uint8_t seen[kEdges];
uint32_t edgeCount = 0;

const uint8_t* current = nullptr;
size_t currentSize = 0;
char artifacts[900] = "./";

using Input = std::vector<uint8_t>;

uint64_t hashOf(const uint8_t* data, size_t size) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < size; ++i) h = (h ^ data[i]) * 1099511628211ull;
    return h;
}

void save(const char* kind) {
    char path[1024];
    size_t at = 0;
    for (const char* p = artifacts; *p && at < 900; ++p) path[at++] = *p;
    for (const char* p = kind; *p; ++p) path[at++] = *p;
    uint64_t h = hashOf(current, currentSize);
    for (int i = 15; i >= 0; --i) path[at++] = "0123456789abcdef"[(h >> (i * 4)) & 15];
    path[at] = 0;
    int file = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (file < 0) return;
    size_t done = 0;
    while (done < currentSize) {
        ssize_t wrote = write(file, current + done, currentSize - done);
        if (wrote <= 0) break;
        done += size_t(wrote);
    }
    close(file);
    static const char note[] = "\nfuzz: input saved as ";
    (void)!write(2, note, sizeof note - 1);
    (void)!write(2, path, at);
    (void)!write(2, "\n", 1);
}

void onSignal(int number) {
    if (current) save(number == SIGALRM ? "timeout-" : "crash-");
    _exit(number == SIGALRM ? 78 : 77);
}

uint64_t resident() {
    mach_task_basic_info_data_t info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, task_info_t(&info), &count) != KERN_SUCCESS) return 0;
    return info.resident_size;
}

bool readFile(const std::string& path, Input& out, size_t limit) {
    FILE* file = fopen(path.c_str(), "rb");
    if (!file) return false;
    out.clear();
    uint8_t block[65536];
    size_t got;
    while ((got = fread(block, 1, sizeof block, file)) > 0 && out.size() < limit) out.insert(out.end(), block, block + std::min(got, limit - out.size()));
    fclose(file);
    return true;
}

bool isFolder(const std::string& path) {
    struct stat status;
    return stat(path.c_str(), &status) == 0 && S_ISDIR(status.st_mode);
}

std::vector<std::string> filesIn(const std::string& folder) {
    std::vector<std::string> names;
    DIR* dir = opendir(folder.c_str());
    if (!dir) return names;
    while (dirent* entry = readdir(dir)) {
        std::string path = folder + "/" + entry->d_name;
        if (entry->d_name[0] != '.' && !isFolder(path)) names.push_back(path);
    }
    closedir(dir);
    std::sort(names.begin(), names.end());
    return names;
}

int bucket(uint8_t count) {
    return count == 0 ? 0 : count == 1 ? 1 : count == 2 ? 2 : count == 3 ? 4 : count < 8 ? 8 : count < 16 ? 16 : count < 32 ? 32 : count < 128 ? 64 : 128;
}

struct Fuzzer {
    std::mt19937_64 rng;
    std::vector<Input> corpus;
    size_t maxLength = 400000;
    unsigned timeout = 25;
    uint64_t memoryLimit = 3ull << 30;
    uint64_t runs = 0;
    uint32_t covered = 0;

    size_t pick(size_t bound) { return bound ? size_t(rng() % bound) : 0; }

    bool run(const Input& input) {
        uint32_t used = std::min<uint32_t>(edgeCount + 1, kEdges);
        memset(hits, 0, used);
        current = input.data();
        currentSize = input.size();
        alarm(timeout);
        LLVMFuzzerTestOneInput(input.data(), input.size());
        alarm(0);
        ++runs;
        if (resident() > memoryLimit) {
            save("oom-");
            fprintf(stderr, "fuzz: resident memory %llu MB is over the limit\n", (unsigned long long)(resident() >> 20));
            _exit(79);
        }
        current = nullptr;
        bool fresh = false;
        for (uint32_t i = 0; i < used; ++i) {
            if (!hits[i]) continue;
            uint8_t kind = uint8_t(bucket(hits[i]));
            if (!(seen[i] & kind)) {
                if (!seen[i]) ++covered;
                seen[i] |= kind;
                fresh = true;
            }
        }
        return fresh;
    }

    void mutate(Input& d) {
        static const uint64_t notable[] = {0, 1, 2, 7, 8, 16, 0x7F, 0x80, 0xFF, 0x100, 0x7FFF, 0x8000, 0xFFFF, 0x10000, 0xFFFFFF, 0x1000000, 0x7FFFFFFF, 0x80000000,
            0xFFFFFFFE, 0xFFFFFFFF, 0x100000000ull, 0x7FFFFFFFFFFFFFFFull, 0x8000000000000000ull, 0xFFFFFFFFFFFFFFFFull};
        int rounds = 1 + int(pick(6));
        for (int round = 0; round < rounds; ++round) {
            if (d.empty()) d.push_back(uint8_t(rng()));
            size_t at = pick(d.size());
            switch (pick(13)) {
                case 0: d[at] ^= uint8_t(1u << pick(8)); break;
                case 1: d[at] = uint8_t(rng()); break;
                case 2: d[at] = uint8_t(d[at] + int(pick(33)) - 16); break;
                case 3:
                case 4: {
                    size_t width = size_t(1) << pick(4);
                    uint64_t value = pick(4) ? notable[pick(sizeof notable / sizeof *notable)] : rng();
                    if (pick(3) == 0) value += pick(17) - 8;
                    bool big = pick(2);
                    for (size_t i = 0; i < width && at + i < d.size(); ++i) d[at + i] = uint8_t(value >> (8 * (big ? width - 1 - i : i)));
                    break;
                }
                case 5: {
                    size_t length = 1 + pick(std::min<size_t>(d.size() - at, pick(4) ? 32 : 4096));
                    d.erase(d.begin() + long(at), d.begin() + long(at + length));
                    break;
                }
                case 6: {
                    size_t length = 1 + pick(pick(4) ? 16 : 512);
                    Input fresh(length);
                    uint8_t fill = uint8_t(rng());
                    bool noise = pick(2);
                    for (uint8_t& b : fresh) b = noise ? uint8_t(rng()) : fill;
                    d.insert(d.begin() + long(at), fresh.begin(), fresh.end());
                    break;
                }
                case 7: {
                    size_t from = pick(d.size());
                    size_t length = 1 + pick(std::min<size_t>(d.size() - from, pick(4) ? 64 : 8192));
                    Input piece(d.begin() + long(from), d.begin() + long(from + length));
                    if (pick(2)) d.insert(d.begin() + long(at), piece.begin(), piece.end());
                    else for (size_t i = 0; i < length && at + i < d.size(); ++i) d[at + i] = piece[i];
                    break;
                }
                case 8: d.resize(at + 1); break;
                case 9: {
                    const Input& other = corpus[pick(corpus.size())];
                    if (other.empty()) break;
                    size_t from = pick(other.size());
                    size_t length = 1 + pick(std::min<size_t>(other.size() - from, pick(3) ? 256 : 65536));
                    if (pick(2)) d.insert(d.begin() + long(at), other.begin() + long(from), other.begin() + long(from + length));
                    else {
                        d.resize(std::max(d.size(), at + length));
                        std::copy(other.begin() + long(from), other.begin() + long(from + length), d.begin() + long(at));
                    }
                    break;
                }
                case 10: {
                    const Input& other = corpus[pick(corpus.size())];
                    size_t cut = pick(other.size() + 1);
                    d.resize(at);
                    d.insert(d.end(), other.begin() + long(cut), other.end());
                    break;
                }
                case 11: {
                    size_t length = 1 + pick(std::min<size_t>(d.size() - at, 64));
                    uint8_t fill = pick(2) ? 0xFF : pick(2) ? 0 : uint8_t(rng());
                    std::fill(d.begin() + long(at), d.begin() + long(at + length), fill);
                    break;
                }
                default: {
                    size_t other = pick(d.size());
                    std::swap(d[at], d[other]);
                    break;
                }
            }
            if (d.size() > maxLength) d.resize(maxLength);
        }
    }
};

double clockNow() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

}

extern "C" void __sanitizer_cov_trace_pc_guard_init(uint32_t* start, uint32_t* stop) {
    if (start == stop || *start) return;
    for (uint32_t* guard = start; guard < stop; ++guard) *guard = ++edgeCount;
}

extern "C" void __sanitizer_cov_trace_pc_guard(uint32_t* guard) {
    uint8_t& count = hits[*guard & (kEdges - 1)];
    if (count != 255) ++count;
}

int main(int argc, char** argv) {
    Fuzzer fuzzer;
    double seconds = 0;
    uint64_t diskLimit = 25ull << 20, onDisk = 0;
    uint64_t seed = uint64_t(std::chrono::steady_clock::now().time_since_epoch().count()) ^ (uint64_t(getpid()) << 32);
    std::vector<std::string> folders, files;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto value = [&](const char* name) -> const char* {
            size_t length = strlen(name);
            return arg.compare(0, length, name) == 0 ? arg.c_str() + length : nullptr;
        };
        if (const char* v = value("-max_total_time=")) seconds = atof(v);
        else if (const char* v = value("-max_len=")) fuzzer.maxLength = size_t(atol(v));
        else if (const char* v = value("-timeout=")) fuzzer.timeout = unsigned(atoi(v));
        else if (const char* v = value("-rss_limit_mb=")) fuzzer.memoryLimit = uint64_t(atol(v)) << 20;
        else if (const char* v = value("-corpus_limit_mb=")) diskLimit = uint64_t(atol(v)) << 20;
        else if (const char* v = value("-seed=")) seed = strtoull(v, nullptr, 10);
        else if (const char* v = value("-artifact_prefix=")) snprintf(artifacts, sizeof artifacts, "%s", v);
        else if (arg[0] == '-') fprintf(stderr, "fuzz: option %s is not known, ignored\n", arg.c_str());
        else if (isFolder(arg)) folders.push_back(arg);
        else files.push_back(arg);
    }
    for (int number : {SIGSEGV, SIGBUS, SIGABRT, SIGILL, SIGFPE, SIGTRAP, SIGALRM}) signal(number, onSignal);
    signal(SIGPIPE, SIG_IGN);
    fuzzer.rng.seed(seed);

    if (!files.empty()) {
        for (const std::string& path : files) {
            Input input;
            if (!readFile(path, input, size_t(1) << 30)) {
                fprintf(stderr, "fuzz: cannot read %s\n", path.c_str());
                return 2;
            }
            fuzzer.run(input);
        }
        printf("replayed %zu inputs without a crash, peak memory %llu MB\n", files.size(), (unsigned long long)(resident() >> 20));
        return 0;
    }
    if (folders.empty()) {
        fprintf(stderr, "usage: %s [-max_total_time=s] [-max_len=n] [-timeout=s] [-rss_limit_mb=n] [-seed=n] [-corpus_limit_mb=n] [-artifact_prefix=p] corpus [seeds...]\n       %s file...\n", argv[0], argv[0]);
        return 2;
    }

    size_t loaded = 0;
    for (const std::string& folder : folders) {
        for (const std::string& path : filesIn(folder)) {
            Input input;
            if (!readFile(path, input, fuzzer.maxLength)) continue;
            ++loaded;
            if (folder == folders[0]) onDisk += input.size();
            if (fuzzer.run(input) || !edgeCount || fuzzer.corpus.empty()) fuzzer.corpus.push_back(std::move(input));
        }
    }
    if (fuzzer.corpus.empty()) fuzzer.corpus.push_back(Input{0});
    fprintf(stderr, "fuzz: %zu inputs read, %zu kept, %u of %u edges covered%s\n", loaded, fuzzer.corpus.size(), fuzzer.covered, edgeCount,
        edgeCount ? "" : " (no coverage instrumentation: blind mutation)");

    double start = clockNow(), report = start;
    uint64_t found = 0;
    while (seconds <= 0 || clockNow() - start < seconds) {
        size_t a = fuzzer.pick(fuzzer.corpus.size()), b = fuzzer.pick(fuzzer.corpus.size());
        Input input = fuzzer.corpus[fuzzer.pick(3) == 0 ? std::max(a, b) : a];
        fuzzer.mutate(input);
        if (fuzzer.run(input)) {
            ++found;
            char name[32];
            snprintf(name, sizeof name, "/%016llx", (unsigned long long)hashOf(input.data(), input.size()));
            FILE* file = onDisk + input.size() <= diskLimit ? fopen((folders[0] + name).c_str(), "wb") : nullptr;
            if (file) {
                fwrite(input.data(), 1, input.size(), file);
                fclose(file);
                onDisk += input.size();
            }
            fuzzer.corpus.push_back(std::move(input));
        }
        if (clockNow() - report >= 30) {
            report = clockNow();
            fprintf(stderr, "fuzz: %.0f s, %llu runs, %.0f per second, corpus %zu (+%llu), edges %u, memory %llu MB\n", report - start, (unsigned long long)fuzzer.runs,
                double(fuzzer.runs) / (report - start), fuzzer.corpus.size(), (unsigned long long)found, fuzzer.covered, (unsigned long long)(resident() >> 20));
        }
    }
    fprintf(stderr, "fuzz: done, %llu runs in %.0f s, corpus %zu (+%llu), edges %u of %u\n", (unsigned long long)fuzzer.runs, clockNow() - start, fuzzer.corpus.size(),
        (unsigned long long)found, fuzzer.covered, edgeCount);
    return 0;
}

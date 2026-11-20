#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(__linux__)
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

constexpr const char* APP_VERSION = "1.0.0";
constexpr size_t BUFSIZE = 65536;
constexpr uintmax_t SAMPLE_THRESHOLD = 4 * 1024 * 1024;
constexpr size_t SAMPLE_BYTES = 65536;

struct FileInfo {
    fs::path path;
    uintmax_t size = 0;
};

struct ScanStats {
    size_t scanned_files = 0;
    size_t unreadable_files = 0;
    size_t skipped_entries = 0;
    size_t scan_warnings = 0;
};

struct Options {
    fs::path root;
    fs::path move_to;
    bool recursive = false;
    bool delete_dups = false;
    bool move_dups = false;
    bool dry_run = false;
};

enum class ParseStatus {
    Ok,
    Help,
    Version,
    Error
};

uint64_t fnv1a_step(uint64_t h, const char* data, size_t len) {
    constexpr uint64_t FNV_PRIME = 1099511628211ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= static_cast<unsigned char>(data[i]);
        h *= FNV_PRIME;
    }
    return h;
}

bool compute_hash(const fs::path& p, uintmax_t expected_size, bool sample_only, uint64_t& out_hash) {
    std::error_code ec;
    auto actual_size = fs::file_size(p, ec);
    if (ec || actual_size != expected_size) return false;

    if (expected_size == 0) {
        out_hash = 14695981039346656037ULL;
        return true;
    }

    std::ifstream f(p, std::ios::binary);
    if (!f) return false;

    uint64_t h = 14695981039346656037ULL;
    char buf[BUFSIZE];

    if (sample_only && actual_size > SAMPLE_BYTES * 2) {
        size_t head_to_read = std::min<size_t>(SAMPLE_BYTES, sizeof(buf));
        f.read(buf, static_cast<std::streamsize>(head_to_read));
        auto n = f.gcount();
        if (f.bad() || n != static_cast<std::streamsize>(head_to_read)) return false;
        h = fnv1a_step(h, buf, static_cast<size_t>(n));

        uintmax_t tail_u64 = actual_size - SAMPLE_BYTES;
        if (tail_u64 > static_cast<uintmax_t>(std::numeric_limits<std::streamoff>::max())) {
            return false;
        }
        auto tail_offset = static_cast<std::streamoff>(tail_u64);
        f.clear();
        f.seekg(tail_offset, std::ios::beg);
        if (!f) return false;

        size_t tail_read = 0;
        while (tail_read < SAMPLE_BYTES) {
            size_t want = std::min<size_t>(sizeof(buf), SAMPLE_BYTES - tail_read);
            f.read(buf, static_cast<std::streamsize>(want));
            auto tn = f.gcount();
            if (f.bad() || tn <= 0) return false;
            h = fnv1a_step(h, buf, static_cast<size_t>(tn));
            tail_read += static_cast<size_t>(tn);
        }
        if (tail_read != SAMPLE_BYTES) return false;

        auto final_size = fs::file_size(p, ec);
        if (ec || final_size != expected_size) return false;

        out_hash = h;
        return true;
    }

    uintmax_t total_read = 0;
    while (true) {
        f.read(buf, static_cast<std::streamsize>(sizeof(buf)));
        auto n = f.gcount();
        if (f.bad()) return false;
        if (n <= 0) break;
        h = fnv1a_step(h, buf, static_cast<size_t>(n));
        total_read += static_cast<uintmax_t>(n);
    }

    auto final_size = fs::file_size(p, ec);
    if (ec || final_size != expected_size || total_read != expected_size) return false;

    out_hash = h;
    return true;
}

bool paths_equal_bytes(const fs::path& path_a, const fs::path& path_b, bool check_expected_size, uintmax_t expected_size) {
    std::error_code ec_a, ec_b;
    auto sz_a = fs::file_size(path_a, ec_a);
    auto sz_b = fs::file_size(path_b, ec_b);
    if (ec_a || ec_b || sz_a != sz_b) {
        return false;
    }
    if (check_expected_size && (sz_a != expected_size || sz_b != expected_size)) {
        return false;
    }

    if (sz_a > 0) {
        std::ifstream fa(path_a, std::ios::binary);
        std::ifstream fb(path_b, std::ios::binary);
        if (!fa || !fb) return false;

        char ba[BUFSIZE], bb[BUFSIZE];
        while (true) {
            fa.read(ba, static_cast<std::streamsize>(sizeof(ba)));
            fb.read(bb, static_cast<std::streamsize>(sizeof(bb)));
            auto na = fa.gcount();
            auto nb = fb.gcount();
            if (fa.bad() || fb.bad()) return false;
            if (na != nb) return false;
            if (na == 0) {
                if (!(fa.eof() && fb.eof())) return false;
                break;
            }
            if (std::memcmp(ba, bb, static_cast<size_t>(na)) != 0) return false;
        }
    }

    std::error_code post_ec_a, post_ec_b;
    auto post_sz_a = fs::file_size(path_a, post_ec_a);
    auto post_sz_b = fs::file_size(path_b, post_ec_b);
    if (post_ec_a || post_ec_b || post_sz_a != sz_a || post_sz_b != sz_b || post_sz_a != post_sz_b) {
        return false;
    }
    return true;
}

bool is_same_or_inside(const fs::path& candidate, const fs::path& base, bool& result, std::string& err) {
    std::error_code ec;
    fs::path c = fs::weakly_canonical(candidate, ec);
    if (ec) {
        err = "could not resolve path '" + candidate.string() + "'";
        return false;
    }
    fs::path b = fs::weakly_canonical(base, ec);
    if (ec) {
        err = "could not resolve path '" + base.string() + "'";
        return false;
    }

    if (c == b) {
        result = true;
        return true;
    }

    fs::path rel = c.lexically_relative(b);
    result = (!rel.empty() && rel != "." && !rel.is_absolute() && *rel.begin() != "..");
    return true;
}

bool entry_exists_no_follow(const fs::path& p, bool& exists) {
    std::error_code ec;
    auto st = fs::symlink_status(p, ec);
    if (ec) {
        if (ec == std::errc::no_such_file_or_directory || ec == std::errc::not_a_directory) {
            exists = false;
            return true;
        }
        return false;
    }
    exists = (st.type() != fs::file_type::not_found);
    return true;
}

bool pick_unique_dest(const fs::path& dest_dir, const fs::path& src, fs::path& out_dest) {
    bool ex = false;
    fs::path candidate = dest_dir / src.filename();
    if (!entry_exists_no_follow(candidate, ex)) return false;
    if (!ex) {
        out_dest = candidate;
        return true;
    }

    std::string stem = src.stem().string();
    std::string ext = src.extension().string();
    for (int n = 1;; n++) {
        fs::path next = dest_dir / (stem + "_" + std::to_string(n) + ext);
        if (!entry_exists_no_follow(next, ex)) return false;
        if (!ex) {
            out_dest = next;
            return true;
        }
    }
}

bool move_file_safely(const fs::path& src, const fs::path& dest_dir) {
    for (int attempt = 0; attempt < 16; attempt++) {
        fs::path dest;
        if (!pick_unique_dest(dest_dir, src, dest)) {
            std::cerr << "error: could not inspect destination directory for " << src << "\n";
            return false;
        }

#if defined(__linux__) && defined(SYS_renameat2) && defined(RENAME_NOREPLACE)
        long rc = syscall(SYS_renameat2, AT_FDCWD, src.c_str(), AT_FDCWD, dest.c_str(), RENAME_NOREPLACE);
        if (rc == 0) return true;
        if (errno == EEXIST) continue;
        if (errno != EINVAL && errno != ENOSYS && errno != EXDEV) {
            std::cerr << "error: failed to move " << src << " to " << dest
                      << ": " << std::strerror(errno) << "\n";
            return false;
        }
        if (errno != EXDEV) {
            std::error_code ex_ec;
            if (fs::exists(dest, ex_ec)) continue;
            std::error_code ren_ec;
            fs::rename(src, dest, ren_ec);
            if (!ren_ec) return true;
            if (ren_ec == std::errc::file_exists) continue;
            if (ren_ec != std::errc::cross_device_link) {
                std::cerr << "error: failed to move " << src << " to " << dest << ": " << ren_ec.message() << "\n";
                return false;
            }
        }
#else
        std::error_code ex_ec;
        if (fs::exists(dest, ex_ec)) continue;
        std::error_code ren_ec;
        fs::rename(src, dest, ren_ec);
        if (!ren_ec) return true;
        if (ren_ec == std::errc::file_exists) continue;
        if (ren_ec != std::errc::cross_device_link) {
            std::cerr << "error: failed to move " << src << " to " << dest << ": " << ren_ec.message() << "\n";
            return false;
        }
#endif

        std::error_code copy_ec;
        if (!fs::copy_file(src, dest, fs::copy_options::none, copy_ec)) {
            if (copy_ec == std::errc::file_exists) {
                continue;
            }
            std::cerr << "error: failed to copy " << src << " to " << dest << ": " << copy_ec.message() << "\n";
            return false;
        }

        if (!paths_equal_bytes(src, dest, false, 0)) {
            std::error_code cleanup_ec;
            fs::remove(dest, cleanup_ec);
            std::cerr << "warning: copied " << src << " to " << dest
                      << ", but verification failed; keeping original\n";
            return false;
        }

        std::error_code rm_ec;
        fs::remove(src, rm_ec);
        if (rm_ec) {
            std::cerr << "warning: copied " << src << " to " << dest
                      << ", but could not remove original (kept both): " << rm_ec.message() << "\n";
            return false;
        }
        return true;
    }

    std::cerr << "error: too many destination collisions while moving " << src << "\n";
    return false;
}

void print_help() {
    std::cout
        << "Usage: dupfinder [options] <directory>\n\n"
        << "Find duplicate files by size, content hash, and byte-for-byte comparison.\n\n"
        << "Options:\n"
        << "  -r, --recursive     Scan subdirectories\n"
        << "  --delete            Delete duplicate files (keeps first sorted path)\n"
        << "  --move <directory>  Move duplicate files into <directory>\n"
        << "  --dry-run           Preview actions without modifying files\n"
        << "  -h, --help          Show this help message\n"
        << "  -V, --version       Show version information\n\n"
        << "Exit codes:\n"
        << "  0  Success (or help/version displayed)\n"
        << "  1  Invalid arguments, filesystem error, scan warnings/unreadable files, or failed operations\n";
}

ParseStatus parse_args(int argc, char** argv, Options& opt) {
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_help();
            return ParseStatus::Help;
        }
        if (arg == "-V" || arg == "--version") {
            std::cout << "dupfinder " << APP_VERSION << "\n";
            return ParseStatus::Version;
        }
        if (arg == "-r" || arg == "--recursive") {
            opt.recursive = true;
        } else if (arg == "--delete") {
            opt.delete_dups = true;
        } else if (arg == "--dry-run") {
            opt.dry_run = true;
        } else if (arg == "--move") {
            if (i + 1 >= argc) {
                std::cerr << "error: --move requires a destination directory\n";
                return ParseStatus::Error;
            }
            opt.move_dups = true;
            opt.move_to = argv[++i];
        } else if (!arg.empty() && arg[0] != '-') {
            if (!opt.root.empty()) {
                std::cerr << "error: only one target directory is supported\n";
                return ParseStatus::Error;
            }
            opt.root = arg;
        } else {
            std::cerr << "error: unknown option '" << arg << "'\n";
            return ParseStatus::Error;
        }
    }

    if (opt.root.empty()) {
        std::cerr << "error: missing target directory\n";
        return ParseStatus::Error;
    }
    if (opt.delete_dups && opt.move_dups) {
        std::cerr << "error: --delete and --move cannot be used together\n";
        return ParseStatus::Error;
    }
    return ParseStatus::Ok;
}

std::vector<FileInfo> gather_files(const Options& opt, ScanStats& stats) {
    std::vector<FileInfo> files;
    std::error_code ec;

    auto handle_entry = [&](const fs::directory_entry& e) {
        std::error_code st_ec;
        bool sym = e.is_symlink(st_ec);
        if (st_ec) {
            stats.scan_warnings++;
            stats.skipped_entries++;
            std::cerr << "warning: failed to inspect " << e.path() << ": " << st_ec.message() << "\n";
            return;
        }
        if (sym) {
            stats.skipped_entries++;
            return;
        }

        bool is_dir = e.is_directory(st_ec);
        if (st_ec) {
            stats.scan_warnings++;
            stats.skipped_entries++;
            std::cerr << "warning: failed directory check on " << e.path() << ": " << st_ec.message() << "\n";
            return;
        }

        if (is_dir) {
            return;
        }

        bool reg = e.is_regular_file(st_ec);
        if (st_ec) {
            stats.scan_warnings++;
            stats.unreadable_files++;
            std::cerr << "warning: cannot stat " << e.path() << ": " << st_ec.message() << "\n";
            return;
        }
        if (!reg) {
            stats.skipped_entries++;
            return;
        }

        auto sz = e.file_size(st_ec);
        if (st_ec) {
            stats.scan_warnings++;
            stats.unreadable_files++;
            std::cerr << "warning: cannot read size of " << e.path() << ": " << st_ec.message() << "\n";
            return;
        }

        stats.scanned_files++;
        files.push_back(FileInfo{e.path(), sz});
    };

    if (opt.recursive) {
        fs::recursive_directory_iterator it(opt.root, fs::directory_options::skip_permission_denied, ec);
        if (ec) {
            stats.scan_warnings++;
            std::cerr << "warning: error opening " << opt.root << ": " << ec.message() << "\n";
            return files;
        }

        fs::recursive_directory_iterator end;
        while (it != end) {
            handle_entry(*it);
            ec.clear();
            it.increment(ec);
            if (ec) {
                stats.scan_warnings++;
                std::cerr << "warning: iterator error during scan: " << ec.message() << "\n";
                break;
            }
        }
    } else {
        fs::directory_iterator it(opt.root, fs::directory_options::skip_permission_denied, ec);
        if (ec) {
            stats.scan_warnings++;
            std::cerr << "warning: error opening " << opt.root << ": " << ec.message() << "\n";
            return files;
        }

        fs::directory_iterator end;
        while (it != end) {
            handle_entry(*it);
            ec.clear();
            it.increment(ec);
            if (ec) {
                stats.scan_warnings++;
                std::cerr << "warning: iterator error during scan: " << ec.message() << "\n";
                break;
            }
        }
    }

    return files;
}

int main(int argc, char** argv) {
    Options opt;
    ParseStatus status = parse_args(argc, argv, opt);
    if (status == ParseStatus::Help || status == ParseStatus::Version) return 0;
    if (status == ParseStatus::Error) return 1;

    std::error_code ec;
    if (!fs::exists(opt.root, ec) || !fs::is_directory(opt.root, ec)) {
        std::cerr << "error: target is not an accessible directory: " << opt.root << "\n";
        return 1;
    }

    if (opt.move_dups) {
        bool overlap = false;
        std::string err;
        if (!is_same_or_inside(opt.move_to, opt.root, overlap, err)) {
            std::cerr << "error: " << err << "\n";
            return 1;
        }
        if (overlap) {
            std::cerr << "error: destination directory cannot be inside or equal to source directory\n";
            return 1;
        }
        if (fs::exists(opt.move_to, ec) && !fs::is_directory(opt.move_to, ec)) {
            std::cerr << "error: destination path exists and is not a directory: " << opt.move_to << "\n";
            return 1;
        }
    }

    ScanStats stats;
    auto files = gather_files(opt, stats);

    std::unordered_map<uintmax_t, std::vector<FileInfo>> by_size;
    by_size.reserve(files.size());
    for (auto& f : files) {
        by_size[f.size].push_back(std::move(f));
    }

    std::vector<std::vector<FileInfo>> dups;

    for (auto& [sz, group] : by_size) {
        if (group.size() < 2) continue;

        bool use_sample = (sz >= SAMPLE_THRESHOLD);
        std::unordered_map<uint64_t, std::vector<FileInfo>> by_hash;
        by_hash.reserve(group.size());

        for (auto& f : group) {
            uint64_t h = 0;
            if (!compute_hash(f.path, f.size, use_sample, h)) {
                stats.unreadable_files++;
                std::cerr << "warning: could not read " << f.path << "\n";
                continue;
            }
            by_hash[h].push_back(std::move(f));
        }

        for (auto& [h, bucket] : by_hash) {
            if (bucket.size() < 2) continue;

            std::vector<std::vector<FileInfo>> clusters;
            for (auto& f : bucket) {
                bool matched = false;
                for (auto& c : clusters) {
                    if (paths_equal_bytes(c[0].path, f.path, true, f.size)) {
                        c.push_back(std::move(f));
                        matched = true;
                        break;
                    }
                }
                if (!matched) {
                    clusters.push_back(std::vector<FileInfo>{std::move(f)});
                }
            }

            for (auto& c : clusters) {
                if (c.size() >= 2) {
                    std::sort(c.begin(), c.end(), [](const FileInfo& a, const FileInfo& b) {
                        return a.path < b.path;
                    });
                    dups.push_back(std::move(c));
                }
            }
        }
    }

    bool had_scan_issues = (stats.unreadable_files > 0 || stats.scan_warnings > 0);

    std::cout << "summary: " << stats.scanned_files << " scanned, "
              << stats.unreadable_files << " unreadable, "
              << stats.skipped_entries << " skipped";
    if (stats.scan_warnings > 0) {
        std::cout << " (" << stats.scan_warnings << " warnings)";
    }
    std::cout << "\n";

    if (dups.empty()) {
        std::cout << "no duplicates found\n";
        return had_scan_issues ? 1 : 0;
    }

    size_t extra = 0;
    double mb = 0.0;
    for (size_t i = 0; i < dups.size(); i++) {
        const auto& g = dups[i];
        std::cout << "\ngroup " << (i + 1) << " (" << g[0].size << " bytes)\n";
        for (size_t j = 0; j < g.size(); j++) {
            std::cout << (j == 0 ? "  keep  " : "  dup   ") << g[j].path << "\n";
        }
        extra += g.size() - 1;
        mb += static_cast<double>(g[0].size) * static_cast<double>(g.size() - 1) / (1024.0 * 1024.0);
    }

    std::cout << "\n" << dups.size() << " groups, " << extra
              << " duplicates, " << std::fixed << std::setprecision(1)
              << mb << " MB reclaimable\n";

    if (!opt.delete_dups && !opt.move_dups) {
        return had_scan_issues ? 1 : 0;
    }

    const char* action = opt.delete_dups ? "delete" : "move";

    if (opt.dry_run) {
        std::cout << "\ndry run: would " << action << ":\n";
        for (const auto& g : dups) {
            for (size_t i = 1; i < g.size(); i++) {
                std::cout << "  " << g[i].path << "\n";
            }
        }
        return had_scan_issues ? 1 : 0;
    }

    std::cout << "\n" << action << " " << extra << " files";
    if (opt.move_dups) std::cout << " to " << opt.move_to;
    std::cout << "? [y/N] ";

    std::string ans;
    if (!std::getline(std::cin, ans) || ans.empty() || (ans[0] != 'y' && ans[0] != 'Y')) {
        std::cout << "cancelled\n";
        return had_scan_issues ? 1 : 0;
    }

    if (opt.move_dups) {
        fs::create_directories(opt.move_to, ec);
        if (ec) {
            std::cerr << "error: could not create destination directory " << opt.move_to
                      << ": " << ec.message() << "\n";
            return 1;
        }
    }

    int ok = 0;
    int fail = 0;
    for (const auto& g : dups) {
        for (size_t i = 1; i < g.size(); i++) {
            if (!paths_equal_bytes(g[0].path, g[i].path, true, g[0].size)) {
                std::cerr << "warning: file changed, skipping " << g[i].path << "\n";
                fail++;
                continue;
            }

            bool success = false;
            if (opt.delete_dups) {
                std::error_code rm_ec;
                success = fs::remove(g[i].path, rm_ec);
                if (!success) {
                    std::cerr << "error: failed to delete " << g[i].path << ": " << rm_ec.message() << "\n";
                }
            } else {
                success = move_file_safely(g[i].path, opt.move_to);
            }

            if (success) ok++;
            else fail++;
        }
    }

    std::cout << ok << " succeeded, " << fail << " failed\n";
    return (fail > 0 || had_scan_issues) ? 1 : 0;
}
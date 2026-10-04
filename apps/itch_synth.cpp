// itch_synth: write a seeded synthetic ITCH 5.0 file.
//
//   itch_synth [--seed N] [--messages N] [--symbols N] [--live N] <output file>
//
// The output is well formed and internally consistent but is not a market
// model. See obe/gen/synthetic_feed.hpp for what it is and is not good for.

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "obe/gen/synthetic_feed.hpp"

namespace {

bool parse_u64(std::string_view text, std::uint64_t& out) {
    const auto r = std::from_chars(text.data(), text.data() + text.size(), out);
    return r.ec == std::errc{} && r.ptr == text.data() + text.size();
}

int usage(std::FILE* to, int status) {
    std::fprintf(to,
                 "usage: itch_synth [--seed N] [--messages N] [--symbols N] [--live N] <output>\n"
                 "  --seed N      PRNG seed (default 1). The same options give the same bytes.\n"
                 "  --messages N  messages after the opening preamble (default 100000)\n"
                 "  --symbols N   number of securities (default 16)\n"
                 "  --live N      resting orders the stream hovers around (default 2000)\n");
    return status;
}

int run(int argc, char** argv) {
    obe::gen::SyntheticConfig cfg;
    std::string output;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        std::uint64_t value = 0;
        const bool has_value = i + 1 < argc && parse_u64(argv[i + 1], value);
        if (arg == "-h" || arg == "--help") {
            return usage(stdout, 0);
        }
        if (arg == "--seed" && has_value) {
            cfg.seed = value;
            ++i;
        } else if (arg == "--messages" && has_value) {
            cfg.messages = value;
            ++i;
        } else if (arg == "--symbols" && has_value) {
            cfg.symbols = static_cast<std::uint32_t>(value);
            ++i;
        } else if (arg == "--live" && has_value) {
            cfg.target_live_orders = static_cast<std::uint32_t>(value);
            ++i;
        } else if (!arg.starts_with("-") && output.empty()) {
            output = arg;
        } else {
            std::fprintf(stderr, "error: bad argument '%s'\n", argv[i]);
            return usage(stderr, 1);
        }
    }
    if (output.empty()) {
        return usage(stderr, 1);
    }

    const std::vector<std::byte> feed = obe::gen::make_synthetic_feed(cfg);
    std::ofstream file(output, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(feed.data()),
               static_cast<std::streamsize>(feed.size()));
    file.close();
    if (!file) {
        std::fprintf(stderr, "error: cannot write '%s'\n", output.c_str());
        return 1;
    }
    std::printf("wrote %zu bytes to %s (seed %llu)\n", feed.size(), output.c_str(),
                static_cast<unsigned long long>(cfg.seed));
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}

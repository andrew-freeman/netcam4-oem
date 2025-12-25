#include "rawabi_network.h"
#include "rawabi_preview.h"

#include <arpa/inet.h>
#include <chrono>
#include <cstdio>
#include <getopt.h>
#include <iostream>
#include <thread>
#include <unistd.h>

using namespace rawabi;

namespace {
struct Args {
    uint16_t port = 10000;
    ViewMode mode = ViewMode::HALF_RES;
    IspConfig isp{};
    bool display = true;
    std::string record_prefix;
};

ViewMode parse_view(const std::string &s) {
    if (s == "mono") return ViewMode::MONO;
    if (s == "green") return ViewMode::GREEN;
    if (s == "half") return ViewMode::HALF_RES;
    if (s == "bilinear") return ViewMode::BILINEAR;
    return ViewMode::HALF_RES;
}

bool parse(int argc, char **argv, Args &args) {
    static option opts[] = {
        {"port", required_argument, nullptr, 'p'},
        {"view", required_argument, nullptr, 'v'},
        {"black", required_argument, nullptr, 'b'},
        {"wb", required_argument, nullptr, 'w'},
        {"gamma", required_argument, nullptr, 'g'},
        {"no-display", no_argument, nullptr, 'n'},
        {"record", required_argument, nullptr, 'r'},
        {nullptr, 0, nullptr, 0},
    };
    int c;
    while ((c = getopt_long(argc, argv, "", opts, nullptr)) != -1) {
        switch (c) {
        case 'p': args.port = static_cast<uint16_t>(std::stoi(optarg)); break;
        case 'v': args.mode = parse_view(optarg); break;
        case 'b': args.isp.black_level = static_cast<uint16_t>(std::stoi(optarg)); break;
        case 'w': std::sscanf(optarg, "%f,%f,%f", &args.isp.wb_r, &args.isp.wb_g, &args.isp.wb_b); break;
        case 'g': args.isp.gamma = std::stof(optarg); break;
        case 'n': args.display = false; break;
        case 'r': args.record_prefix = optarg; break;
        default:
            return false;
        }
    }
    return true;
}

void log_stats(const StatsSnapshot &snap) {
    std::cout << "fps=" << snap.fps << " Mbps=" << snap.mbps
              << " dropped=" << snap.frames_dropped
              << " drop_rate=" << snap.drop_rate
              << " reorder=" << snap.reorder_depth
              << " latency_ms=" << snap.latency_ms << std::endl;
}

} // namespace

int main(int argc, char **argv) {
    Args args;
    if (!parse(argc, argv, args)) {
        std::cerr << "Usage: rawabi_receiver [--port 10000] [--view mono|green|half|bilinear]\n"
                     "                      [--black 0] [--wb r,g,b] [--gamma 2.2] [--no-display]\n"
                     "                      [--record prefix]\n";
        return 1;
    }

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(args.port);
    if (bind(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }

    FrameReassembler reasm;
    std::vector<uint8_t> buf(9000);
    auto last_log = std::chrono::steady_clock::now();
    uint64_t frames_since_log = 0;
    uint64_t bytes_since_log = 0;
    StatsSnapshot overlay{};

    while (true) {
        ssize_t r = recvfrom(sock, buf.data(), buf.size(), 0, nullptr, nullptr);
        if (r <= 0) {
            continue;
        }
        auto completed = reasm.ingest(buf.data(), static_cast<std::size_t>(r));
        for (auto &f : completed) {
            frames_since_log++;
            bytes_since_log += f.payload.size();
            if (!args.record_prefix.empty()) {
                char path[256];
                std::snprintf(path, sizeof(path), "%s_%08u.raw", args.record_prefix.c_str(), f.fseq32);
                FILE *fp = std::fopen(path, "wb");
                if (fp) {
                    std::fwrite(f.payload.data(), 1, f.payload.size(), fp);
                    std::fclose(fp);
                }
            }
            if (args.display) {
                if (f.timestamp) {
                    uint64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count();
                    overlay.latency_ms = (now_us > f.timestamp) ? (now_us - f.timestamp) / 1000.0 : 0.0;
                }
                auto pf = render_preview(f, args.isp, args.mode);
#ifdef RAWABI_HAVE_OPENCV
                display_frame(pf, "rawabi", overlay, false);
#endif
            }
        }

        auto now = std::chrono::steady_clock::now();
        if (now - last_log > std::chrono::seconds(1)) {
            double secs = std::chrono::duration<double>(now - last_log).count();
            auto s = reasm.stats();
            overlay.frames_dropped = s.frames_dropped;
            overlay.frames_completed = s.frames_completed;
            overlay.reorder_depth = s.reorder_depth;
            overlay.fps = frames_since_log / secs;
            overlay.mbps = (bytes_since_log * 8.0) / (secs * 1e6);
            uint64_t total = overlay.frames_completed + overlay.frames_dropped;
            overlay.drop_rate = total ? static_cast<double>(overlay.frames_dropped) / total : 0.0;
            log_stats(overlay);
            frames_since_log = 0;
            bytes_since_log = 0;
            last_log = now;
        }
    }

    return 0;
}

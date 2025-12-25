#include "rawabi_generator.h"
#include "rawabi_network.h"

#include <arpa/inet.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <getopt.h>
#include <iostream>
#include <thread>
#include <time.h>
#include <unistd.h>

using namespace rawabi;

namespace {

enum class SourceMode { GENERATOR, FILE_REPLAY };

struct Cmd {
    std::string dst_ip = "127.0.0.1";
    uint16_t dst_port = 10000;
    uint16_t width = 640;
    uint16_t height = 480;
    uint32_t fps = 30;
    enum sample_format fmt = sf_12bit;
    BayerPattern pattern = BayerPattern::BAYER_RG1BG2;
    uint32_t frames = 0;
    uint32_t flow_id = 1;
    uint16_t fragment = 1400;
    double loss = 0.0;
    double dup = 0.0;
    uint32_t reorder = 0;
    Pattern gen_pattern = Pattern::COLOR_BARS;
    SourceMode mode = SourceMode::GENERATOR;
    std::string raw_file;
};

enum sample_format fmt_from_bits(uint32_t bits) {
    switch (bits) {
    case 8: return sf_8bit;
    case 10: return sf_10bit;
    case 12: return sf_12bit;
    case 14: return sf_14bit;
    case 16: return sf_16bit;
    default: return sf_12bit;
    }
}

Pattern parse_pattern(const std::string &s) {
    if (s == "bars") return Pattern::COLOR_BARS;
    if (s == "ramp") return Pattern::RAMP;
    if (s == "checker") return Pattern::CHECKER;
    if (s == "box") return Pattern::MOVING_BOX;
    if (s == "slanted") return Pattern::SLANTED_EDGE;
    return Pattern::COLOR_BARS;
}

uint64_t monotonic_us() {
    struct timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000ull +
           static_cast<uint64_t>(ts.tv_nsec) / 1000ull;
}

std::vector<uint8_t> pack_payload(const std::vector<uint16_t> &pixels, uint8_t bits) {
    std::size_t bpp = (bits + 7u) / 8u;
    if (bpp == 0) bpp = 1;
    std::vector<uint8_t> out(pixels.size() * bpp);
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        if (bpp == 1) {
            out[i] = static_cast<uint8_t>(pixels[i] & 0xFFu);
        } else {
            std::memcpy(&out[i * bpp], &pixels[i], bpp);
        }
    }
    return out;
}

void usage(const char *prog) {
    std::fprintf(stderr,
                 "%s --dest <ip> [--port 10000] [--width 640] [--height 480]\n"
                 "           [--fps 30] [--bit-depth 12] [--bayer rg1bg2] [--pattern bars]\n"
                 "           [--frames N] [--fragment 1400] [--flow 1] [--loss PCT] [--dup PCT] [--reorder N]\n"
                 "           [--raw-file path]\n",
                 prog);
}

bool parse(int argc, char **argv, Cmd &cmd) {
    static struct option opts[] = {
        {"dest", required_argument, nullptr, 'd'},
        {"port", required_argument, nullptr, 'p'},
        {"width", required_argument, nullptr, 'w'},
        {"height", required_argument, nullptr, 'h'},
        {"fps", required_argument, nullptr, 'f'},
        {"bit-depth", required_argument, nullptr, 'b'},
        {"bayer", required_argument, nullptr, 'y'},
        {"frames", required_argument, nullptr, 'n'},
        {"pattern", required_argument, nullptr, 't'},
        {"fragment", required_argument, nullptr, 'm'},
        {"flow", required_argument, nullptr, 'L'},
        {"loss", required_argument, nullptr, 'l'},
        {"dup", required_argument, nullptr, 'u'},
        {"reorder", required_argument, nullptr, 'r'},
        {"raw-file", required_argument, nullptr, 'R'},
        {nullptr, 0, nullptr, 0},
    };
    int c;
    while ((c = getopt_long(argc, argv, "", opts, nullptr)) != -1) {
        switch (c) {
        case 'd': cmd.dst_ip = optarg; break;
        case 'p': cmd.dst_port = static_cast<uint16_t>(std::stoi(optarg)); break;
        case 'w': cmd.width = static_cast<uint16_t>(std::stoi(optarg)); break;
        case 'h': cmd.height = static_cast<uint16_t>(std::stoi(optarg)); break;
        case 'f': cmd.fps = static_cast<uint32_t>(std::stoul(optarg)); break;
        case 'b': cmd.fmt = fmt_from_bits(static_cast<uint32_t>(std::stoul(optarg))); break;
        case 'y': {
            auto pat = bayer_from_string(optarg);
            if (pat) cmd.pattern = *pat;
            break;
        }
        case 'n': cmd.frames = static_cast<uint32_t>(std::stoul(optarg)); break;
        case 't': cmd.gen_pattern = parse_pattern(optarg); break;
        case 'm': cmd.fragment = static_cast<uint16_t>(std::stoul(optarg)); break;
        case 'L': cmd.flow_id = static_cast<uint32_t>(std::stoul(optarg)); break;
        case 'l': cmd.loss = std::stod(optarg); break;
        case 'u': cmd.dup = std::stod(optarg); break;
        case 'r': cmd.reorder = static_cast<uint32_t>(std::stoul(optarg)); break;
        case 'R': cmd.mode = SourceMode::FILE_REPLAY; cmd.raw_file = optarg; break;
        default:
            usage(argv[0]);
            return false;
        }
    }
    return true;
}

}

int main(int argc, char **argv) {
    Cmd cmd;
    if (!parse(argc, argv, cmd)) {
        return 1;
    }

    FrameInfo info;
    info.width = cmd.width;
    info.height = cmd.height;
    info.fmt = cmd.fmt;
    info.pattern = cmd.pattern;
    info.flow_id = cmd.flow_id;

    std::vector<uint16_t> frame_buf;
    RawFrameGenerator gen(info, {.pattern = cmd.gen_pattern});
    if (cmd.mode == SourceMode::FILE_REPLAY) {
        frame_buf = RawFrameGenerator::load_from_file(cmd.raw_file, info);
        if (frame_buf.empty()) {
            std::cerr << "Failed to load raw file for replay\n";
            return 1;
        }
    }

    SenderOptions opts;
    opts.destination_ip = cmd.dst_ip;
    opts.destination_port = cmd.dst_port;
    opts.flow_id = info.flow_id;
    opts.fragment_payload = cmd.fragment;
    opts.loss_percent = cmd.loss;
    opts.duplicate_percent = cmd.dup;
    opts.reorder_window = cmd.reorder;

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }

    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(cmd.dst_port);
    if (inet_aton(cmd.dst_ip.c_str(), &dst.sin_addr) == 0) {
        std::cerr << "Invalid destination IP\n";
        return 1;
    }

    uint32_t fseq = 0;
    ReorderBuffer reorder;
    reorder.window = cmd.reorder;
    auto frame_interval = std::chrono::microseconds(cmd.fps ? (1000000u / cmd.fps) : 0);
    auto next_time = std::chrono::steady_clock::now();

    while (cmd.frames == 0 || fseq < cmd.frames) {
        if (cmd.mode == SourceMode::GENERATOR) {
            frame_buf = gen.next_frame(fseq);
        }
        uint8_t bits = sample_bits(info.fmt);
        auto packed = pack_payload(frame_buf, bits);
        auto packets = fragment_frame(info, fseq, static_cast<uint64_t>(monotonic_us()),
                                      packed.data(), packed.size(), opts, reorder);
        for (auto &pkt : packets) {
            ssize_t sent = sendto(sock, pkt.data.data(), pkt.data.size(), 0,
                                  reinterpret_cast<sockaddr *>(&dst), sizeof(dst));
            if (sent < 0) {
                perror("sendto");
                return 1;
            }
        }
        ++fseq;
        if (cmd.fps) {
            next_time += frame_interval;
            std::this_thread::sleep_until(next_time);
        }
    }

    close(sock);
    return 0;
}

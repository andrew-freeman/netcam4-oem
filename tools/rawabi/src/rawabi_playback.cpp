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
struct Args {
    std::string file;
    uint16_t width = 0;
    uint16_t height = 0;
    enum sample_format fmt = sf_12bit;
    BayerPattern pattern = BayerPattern::BAYER_RG1BG2;
    std::string dst_ip = "127.0.0.1";
    uint16_t dst_port = 10000;
    uint32_t fps = 30;
    uint32_t repeat = 0;
    uint16_t fragment = 1400;
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

bool parse(int argc, char **argv, Args &args) {
    static option opts[] = {
        {"file", required_argument, nullptr, 'f'},
        {"width", required_argument, nullptr, 'w'},
        {"height", required_argument, nullptr, 'h'},
        {"bit-depth", required_argument, nullptr, 'b'},
        {"bayer", required_argument, nullptr, 'y'},
        {"dest", required_argument, nullptr, 'd'},
        {"port", required_argument, nullptr, 'p'},
        {"fps", required_argument, nullptr, 'r'},
        {"repeat", required_argument, nullptr, 'n'},
        {"fragment", required_argument, nullptr, 'm'},
        {nullptr, 0, nullptr, 0},
    };
    int c;
    while ((c = getopt_long(argc, argv, "", opts, nullptr)) != -1) {
        switch (c) {
        case 'f': args.file = optarg; break;
        case 'w': args.width = static_cast<uint16_t>(std::stoi(optarg)); break;
        case 'h': args.height = static_cast<uint16_t>(std::stoi(optarg)); break;
        case 'b': args.fmt = fmt_from_bits(static_cast<uint32_t>(std::stoul(optarg))); break;
        case 'y': {
            auto pat = bayer_from_string(optarg);
            if (pat) args.pattern = *pat;
            break;
        }
        case 'd': args.dst_ip = optarg; break;
        case 'p': args.dst_port = static_cast<uint16_t>(std::stoi(optarg)); break;
        case 'r': args.fps = static_cast<uint32_t>(std::stoul(optarg)); break;
        case 'n': args.repeat = static_cast<uint32_t>(std::stoul(optarg)); break;
        case 'm': args.fragment = static_cast<uint16_t>(std::stoul(optarg)); break;
        default: return false;
        }
    }
    return !args.file.empty() && args.width && args.height;
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
}

int main(int argc, char **argv) {
    Args args;
    if (!parse(argc, argv, args)) {
        std::cerr << "Usage: rawabi_playback --file frame.raw --width W --height H --bit-depth 12 --bayer rg1bg2\n"
                     "                      [--dest 127.0.0.1] [--port 10000] [--fps 30] [--repeat 0] [--fragment 1400]\n";
        return 1;
    }

    FrameInfo info;
    info.width = args.width;
    info.height = args.height;
    info.fmt = args.fmt;
    info.pattern = args.pattern;

    auto frame = RawFrameGenerator::load_from_file(args.file, info);
    if (frame.empty()) {
        std::cerr << "Failed to read input raw file\n";
        return 1;
    }

    SenderOptions opts;
    opts.destination_ip = args.dst_ip;
    opts.destination_port = args.dst_port;
    opts.fragment_payload = args.fragment;

    uint8_t bits = sample_bits(info.fmt);
    auto packed = pack_payload(frame, bits);

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(args.dst_port);
    if (inet_aton(args.dst_ip.c_str(), &dst.sin_addr) == 0) {
        std::cerr << "Invalid destination IP\n";
        return 1;
    }

    uint32_t loops = (args.repeat == 0) ? 1 : args.repeat;
    auto interval = std::chrono::microseconds(args.fps ? (1000000u / args.fps) : 0);
    ReorderBuffer reorder;
    for (uint32_t i = 0; i < loops; ++i) {
        auto packets = fragment_frame(info, i, monotonic_us(),
                                      packed.data(), packed.size(), opts, reorder);
        for (auto &pkt : packets) {
            sendto(sock, pkt.data.data(), pkt.data.size(), 0,
                   reinterpret_cast<sockaddr *>(&dst), sizeof(dst));
        }
        if (args.fps) {
            std::this_thread::sleep_for(interval);
        }
    }

    close(sock);
    return 0;
}

#include "rawabi_network.h"

#include <arpa/inet.h>
#include <cstdio>
#include <getopt.h>
#include <iostream>
#include <unistd.h>

using namespace rawabi;

namespace {
struct Args {
    uint16_t port = 10000;
    std::string prefix = "capture";
    uint32_t max_frames = 0;
};

bool parse(int argc, char **argv, Args &args) {
    static option opts[] = {
        {"port", required_argument, nullptr, 'p'},
        {"prefix", required_argument, nullptr, 'o'},
        {"frames", required_argument, nullptr, 'n'},
        {nullptr, 0, nullptr, 0},
    };
    int c;
    while ((c = getopt_long(argc, argv, "", opts, nullptr)) != -1) {
        switch (c) {
        case 'p': args.port = static_cast<uint16_t>(std::stoi(optarg)); break;
        case 'o': args.prefix = optarg; break;
        case 'n': args.max_frames = static_cast<uint32_t>(std::stoul(optarg)); break;
        default: return false;
        }
    }
    return true;
}
}

int main(int argc, char **argv) {
    Args args;
    if (!parse(argc, argv, args)) {
        std::cerr << "Usage: rawabi_record [--port 10000] [--prefix capture] [--frames N]\n";
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
    uint32_t written = 0;

    while (args.max_frames == 0 || written < args.max_frames) {
        ssize_t r = recvfrom(sock, buf.data(), buf.size(), 0, nullptr, nullptr);
        if (r <= 0) continue;
        auto completed = reasm.ingest(buf.data(), static_cast<std::size_t>(r));
        for (auto &f : completed) {
            char path[256];
            std::snprintf(path, sizeof(path), "%s_%08u.raw", args.prefix.c_str(), f.fseq32);
            FILE *fp = std::fopen(path, "wb");
            if (fp) {
                std::fwrite(f.payload.data(), 1, f.payload.size(), fp);
                std::fclose(fp);
                ++written;
                std::cout << "Saved " << path << std::endl;
            }
            if (args.max_frames && written >= args.max_frames) {
                break;
            }
        }
    }

    return 0;
}

#include "rawabi_network.h"
#include "rawabi_generator.h"

#include <arpa/inet.h>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace rawabi;

void test_header_pack() {
    FrameInfo info;
    info.width = 64;
    info.height = 32;
    info.fmt = sf_10bit;
    info.pattern = BayerPattern::BAYER_RG1BG2;

    SenderOptions opts;
    opts.fragment_payload = 100;
    ReorderBuffer reorder;

    std::vector<uint8_t> payload(1024, 0xAA);
    auto packets = fragment_frame(info, 42, 123456, payload.data(), payload.size(), opts, reorder);
    assert(!packets.empty());
    bool has_fh = false;
    bool has_fd = false;
    for (const auto &p : packets) {
        const auto *lid = reinterpret_cast<const uint32_t *>(p.data.data());
        if (*lid & LID_TYPE) {
            has_fh = true;
            auto *fh = reinterpret_cast<const video_frame_raw_hdr_t *>(p.data.data());
            assert(ntohs(fh->x_dim) == info.width);
            assert(ntohs(fh->y_dim) == info.height);
            assert((ntohl(fh->fsize) & 0xF0000000u) == encode_sample_format(info.fmt));
        } else {
            has_fd = true;
            auto *fd = reinterpret_cast<const video_frame_raw_t *>(p.data.data());
            assert(fd->fseq == static_cast<uint8_t>(42));
            assert((ntohl(fd->offs) & 0xF0000000u) == encode_sample_format(info.fmt));
        }
    }
    assert(has_fh && has_fd);
}

void test_reassembly_in_order() {
    FrameInfo info;
    info.width = 8;
    info.height = 4;
    info.fmt = sf_12bit;
    RawFrameGenerator gen(info);
    auto frame = gen.next_frame(0);
    std::vector<uint8_t> payload(frame.size() * sizeof(uint16_t));
    std::memcpy(payload.data(), frame.data(), payload.size());

    SenderOptions opts;
    opts.fragment_payload = 16;
    ReorderBuffer reorder;
    auto packets = fragment_frame(info, 1, 55, payload.data(), payload.size(), opts, reorder);

    FrameReassembler reasm;
    std::vector<CompletedFrame> completed;
    for (auto &pkt : packets) {
        auto out = reasm.ingest(pkt.data.data(), pkt.data.size());
        completed.insert(completed.end(), out.begin(), out.end());
    }
    assert(completed.size() == 1);
    assert(completed[0].payload.size() == payload.size());
    assert(std::memcmp(completed[0].payload.data(), payload.data(), payload.size()) == 0);
}

void test_reassembly_reorder() {
    FrameInfo info;
    info.width = 16;
    info.height = 4;
    info.fmt = sf_8bit;
    std::vector<uint8_t> payload(static_cast<std::size_t>(info.width) * info.height, 0x5A);

    SenderOptions opts;
    opts.fragment_payload = 12;
    ReorderBuffer reorder;
    auto packets = fragment_frame(info, 2, 77, payload.data(), payload.size(), opts, reorder);

    // Reverse FD packets to simulate reorder
    std::vector<FramePacket> reversed;
    FramePacket fh;
    for (auto &p : packets) {
        if (p.is_fh) fh = p; else reversed.push_back(p);
    }
    std::reverse(reversed.begin(), reversed.end());

    FrameReassembler reasm;
    reasm.ingest(fh.data.data(), fh.data.size());
    std::vector<CompletedFrame> completed;
    for (auto &p : reversed) {
        auto out = reasm.ingest(p.data.data(), p.data.size());
        completed.insert(completed.end(), out.begin(), out.end());
    }
    assert(completed.size() == 1);
    assert(completed[0].payload.size() == payload.size());
}

void test_loopback_integration() {
    FrameInfo info;
    info.width = 32;
    info.height = 24;
    info.fmt = sf_10bit;
    RawFrameGenerator gen(info);
    auto frame = gen.next_frame(3);
    std::vector<uint8_t> payload(frame.size() * sizeof(uint16_t));
    std::memcpy(payload.data(), frame.data(), payload.size());

    int recv_sock = socket(AF_INET, SOCK_DGRAM, 0);
    assert(recv_sock >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(0);
    assert(bind(recv_sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);
    socklen_t slen = sizeof(addr);
    getsockname(recv_sock, reinterpret_cast<sockaddr *>(&addr), &slen);
    uint16_t port = ntohs(addr.sin_port);

    FrameReassembler reasm;
    std::vector<CompletedFrame> completed;
    std::thread rx([&]() {
        std::vector<uint8_t> buf(9000);
        for (;;) {
            ssize_t r = recvfrom(recv_sock, buf.data(), buf.size(), 0, nullptr, nullptr);
            if (r <= 0) continue;
            auto out = reasm.ingest(buf.data(), static_cast<std::size_t>(r));
            completed.insert(completed.end(), out.begin(), out.end());
            if (!completed.empty()) break;
        }
    });

    int tx = socket(AF_INET, SOCK_DGRAM, 0);
    assert(tx >= 0);
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(port);
    inet_aton("127.0.0.1", &dst.sin_addr);

    SenderOptions opts;
    opts.destination_ip = "127.0.0.1";
    opts.destination_port = port;
    opts.fragment_payload = 32;
    ReorderBuffer reorder;
    auto packets = fragment_frame(info, 9, 999, payload.data(), payload.size(), opts, reorder);
    for (auto &p : packets) {
        sendto(tx, p.data.data(), p.data.size(), 0, reinterpret_cast<sockaddr *>(&dst), sizeof(dst));
    }

    rx.join();
    close(tx);
    close(recv_sock);
    assert(!completed.empty());
}

int main() {
    test_header_pack();
    test_reassembly_in_order();
    test_reassembly_reorder();
    test_loopback_integration();
    std::cout << "rawabi tests passed" << std::endl;
    return 0;
}

/* Synthetic RAW video sender producing ABI-compatible FH/FD packets.
 *
 * This dummy generator is meant to be drop-in compatible with the existing
 * receivers that consume video_frame_raw_hdr_t (FH) followed by one or more
 * video_frame_raw_t (FD) fragments. When real camera data becomes available,
 * swap synthetic_frame() with the capture callback.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <endian.h>
#include <unistd.h>

#include <abi/ip-video-raw.h>
#include <abi/p3.h>
#include <abi/device.h>

#define DEFAULT_WIDTH  1928
#define DEFAULT_HEIGHT 1090
#define DEFAULT_PORT   10000
#define DEFAULT_CTRL   10001
#define DEFAULT_FLOW   1

#define MTU_SAFE_PAYLOAD 1400u

typedef struct {
	struct sockaddr_in dst;
	int                sock;
	int                ctrl_sock;

	enum {
		PATTERN_GRADIENT = 0,
		PATTERN_FLAT,
		PATTERN_CHECKER,
		PATTERN_NOISE,
	} pattern;

	uint32_t           width;
	uint32_t           height;
	uint32_t           flow_id;
	uint32_t           fragment_payload;
	uint32_t           fps;

	volatile int       run;
	volatile int       stop;
	uint32_t           fseq;
	uint64_t           ts_origin_ns;
} tx_ctx_t;

static uint64_t monotonic_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

static uint32_t lcg_next(uint32_t *state)
{
	*state = (*state * 1664525u + 1013904223u);
	return *state;
}

static void synthetic_frame(tx_ctx_t *ctx, uint16_t *buf, uint32_t fseq)
{
	uint32_t w = ctx->width;
	uint32_t h = ctx->height;

	uint32_t noise_state = 0xdeadbeefu ^ fseq;
	uint16_t flat_val = (uint16_t)(((fseq * 17u) & 0x0fffu) << 4);

	for (uint32_t y = 0; y < h; ++y) {
		for (uint32_t x = 0; x < w; ++x) {
			uint16_t val = 0;
			switch (ctx->pattern) {
			case PATTERN_GRADIENT: {
				uint32_t v = (x + y + fseq * 4u) & 0x0fffu;
				val = (uint16_t)(v << 4);
				break;
			}
			case PATTERN_FLAT:
				val = flat_val;
				break;
			case PATTERN_CHECKER: {
				uint32_t v = (((x >> 4) ^ (y >> 4) ^ fseq) & 1u) ? 0x0ff0u : 0x0100u;
				val = (uint16_t)v;
				break;
			}
			case PATTERN_NOISE: {
				uint32_t v = lcg_next(&noise_state) & 0x0fffu;
				val = (uint16_t)(v << 4);
				break;
			}
			default:
				val = 0;
				break;
			}
			buf[y * w + x] = val;
		}
	}
	/* embed frame counter in a small corner for quick visual checks */
	for (uint32_t i = 0; i < 32 && i < w; ++i) {
		buf[i] = (uint16_t)((fseq & 0x0fffu) << 4);
	}
}

static int send_frame(tx_ctx_t *ctx, uint16_t *frame_buf)
{
	size_t pixel_count = (size_t)ctx->width * ctx->height;
	size_t frame_bytes = pixel_count * sizeof(uint16_t);

	uint32_t fseq32 = ctx->fseq++;
	uint8_t fseq8 = (uint8_t)fseq32;

	/* prepare FH */
	video_frame_raw_hdr_t fh;
	memset(&fh, 0, sizeof(fh));
	fh.lid   = (uint32_t)LID_FH | (ctx->flow_id & 0x7fffffff);
	fh.fseq  = htonl(fseq32);
	fh.ts    = htobe64(monotonic_us() - ctx->ts_origin_ns / 1000ull);
	fh.x_dim = htons((uint16_t)ctx->width);
	fh.y_dim = htons((uint16_t)ctx->height);
	fh.fsize = htonl((uint32_t)(frame_bytes & 0x0fffffff) | sf_16bit);
	fh.osize = 0;

	if (sendto(ctx->sock, &fh, sizeof(fh), 0,
	           (struct sockaddr *)&ctx->dst, sizeof(ctx->dst)) < 0) {
		perror("send FH");
		return -1;
	}

	/* fragment and send FD packets */
	uint32_t offset = 0;
	while (offset < frame_bytes) {
		uint32_t chunk = ctx->fragment_payload;
		if (chunk > frame_bytes - offset) {
			chunk = (uint32_t)(frame_bytes - offset);
		}

		size_t pkt_size = sizeof(video_frame_raw_t) + chunk;
		video_frame_raw_t *fd = calloc(1, pkt_size);
		if (!fd) {
			perror("calloc");
			return -1;
		}

		fd->lid    = (ctx->flow_id & 0x7fffffff) | (uint32_t)LID_FD;
		fd->flags  = 4; /* colour mode: BW */
		fd->fseq   = fseq8;
		fd->size   = htons((uint16_t)chunk);
		fd->x_dim  = htons((uint16_t)ctx->width);
		fd->y_dim  = htons((uint16_t)ctx->height);
		fd->offs   = htonl((offset & 0x0fffffff) | sf_16bit);

		memcpy(fd->data, (uint8_t *)frame_buf + offset, chunk);

		ssize_t sent = sendto(ctx->sock, fd, pkt_size, 0,
		                      (struct sockaddr *)&ctx->dst, sizeof(ctx->dst));
		free(fd);
		if (sent < 0) {
			perror("send FD");
			return -1;
		}

		offset += chunk;
	}

	return 0;
}

static void *ctrl_thread(void *arg)
{
	tx_ctx_t *ctx = arg;
	uint8_t buf[512];
	struct sockaddr_in src;
	socklen_t slen = sizeof(src);

	while (!ctx->stop) {
		ssize_t r = recvfrom(ctx->ctrl_sock, buf, sizeof(buf), 0,
		                     (struct sockaddr *)&src, &slen);
		if (r < 4) {
			continue;
		}
		uint32_t op = ntohl(*(uint32_t *)buf);
		if (op == DEVICE_ABI_START_FLOW_CMD) {
			ctx->run = 1;
		} else if (op == DEVICE_ABI_STOP_FLOW_CMD) {
			ctx->run = 0;
		}
	}

	return NULL;
}

static void usage(const char *prog)
{
	fprintf(stderr,
	        "Usage: %s -d <dst_ip> [-p <dst_port>] [-c <ctrl_port>] "
	        "[-w <width>] [-h <height>] [-f <fps>] [-m <fragment bytes>] "
	        "[-l <flow_id>] [-t pattern]\n"
	        "Patterns: 0=gradient (default), 1=flat, 2=checker, 3=noise\n",
	        prog);
}

static tx_ctx_t g_ctx;

static void handle_sig(int signo)
{
	(void)signo;
	g_ctx.stop = 1;
}

int main(int argc, char **argv)
{
	tx_ctx_t ctx = {
		.width            = DEFAULT_WIDTH,
		.height           = DEFAULT_HEIGHT,
		.pattern          = PATTERN_GRADIENT,
		.flow_id          = DEFAULT_FLOW,
		.fragment_payload = MTU_SAFE_PAYLOAD,
		.fps              = 30,
		.run              = 0,
		.stop             = 0,
		.fseq             = 0,
	};

	g_ctx = ctx;

	const char *dst_ip = NULL;
	uint16_t ctrl_port = DEFAULT_CTRL;
	uint16_t data_port = DEFAULT_PORT;
	int opt;
	while ((opt = getopt(argc, argv, "d:p:c:w:h:f:m:l:t:")) != -1) {
		switch (opt) {
		case 'd':
			dst_ip = optarg;
			break;
		case 'p':
			data_port = (uint16_t)atoi(optarg);
			break;
		case 'c':
			ctrl_port = (uint16_t)atoi(optarg);
			break;
		case 'w':
			ctx.width = (uint32_t)atoi(optarg);
			break;
		case 'h':
			ctx.height = (uint32_t)atoi(optarg);
			break;
		case 'f':
			ctx.fps = (uint32_t)atoi(optarg);
			break;
		case 'm':
			ctx.fragment_payload = (uint32_t)atoi(optarg);
			break;
		case 'l':
			ctx.flow_id = (uint32_t)atoi(optarg);
			break;
		case 't': {
			int p = atoi(optarg);
			if (p < PATTERN_GRADIENT || p > PATTERN_NOISE) {
				usage(argv[0]);
				return 1;
			}
			ctx.pattern = (typeof(ctx.pattern))p;
			break;
		}
		default:
			usage(argv[0]);
			return 1;
		}
	}

	if (!dst_ip) {
		usage(argv[0]);
		return 1;
	}

	if (ctx.fragment_payload < 64 || ctx.fragment_payload > 65000) {
		fprintf(stderr, "Fragment payload out of range\n");
		return 1;
	}

	ctx.dst.sin_family = AF_INET;
	ctx.dst.sin_port = htons(data_port);
	if (inet_aton(dst_ip, &ctx.dst.sin_addr) == 0) {
		fprintf(stderr, "Invalid destination IP\n");
		return 1;
	}

	ctx.sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (ctx.sock < 0) {
		perror("socket");
		return 1;
	}

	ctx.ctrl_sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (ctx.ctrl_sock < 0) {
		perror("ctrl socket");
		return 1;
	}

	struct sockaddr_in ctrl_addr = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_ANY),
		.sin_port = htons(ctrl_port),
	};
	if (bind(ctx.ctrl_sock, (struct sockaddr *)&ctrl_addr, sizeof(ctrl_addr)) < 0) {
		perror("bind ctrl");
		return 1;
	}

	pthread_t ctrl;
	ctx.ts_origin_ns = monotonic_us() * 1000ull;
	pthread_create(&ctrl, NULL, ctrl_thread, &ctx);

	signal(SIGINT, handle_sig);
	signal(SIGTERM, handle_sig);

	size_t frame_bytes = (size_t)ctx.width * ctx.height * sizeof(uint16_t);
	uint16_t *frame = malloc(frame_bytes);
	if (!frame) {
		perror("malloc frame");
		return 1;
	}

	uint32_t frame_interval_us = (ctx.fps > 0) ? (1000000u / ctx.fps) : 0;
	while (!ctx.stop) {
		if (ctx.run) {
			synthetic_frame(&ctx, frame, ctx.fseq);
			if (send_frame(&ctx, frame) != 0) {
				break;
			}
		}
		if (frame_interval_us) {
			usleep(frame_interval_us);
		} else {
			usleep(10000);
		}
	}

	free(frame);
	ctx.run = 0;
	ctx.stop = 1;
	pthread_join(ctrl, NULL);
	close(ctx.sock);
	close(ctx.ctrl_sock);
	return 0;
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <linux/videodev2.h>

#define VIDEO_DEVICE "/dev/video1"
#define WIDTH 640
#define HEIGHT 480
#define PIX_FMT V4L2_PIX_FMT_YUYV
#define FPS 15  // 降低帧率，提升嵌入式稳定性（从30→15
#define BUFFER_COUNT 2
#define YUYV_FRAME_SIZE (WIDTH * HEIGHT * 2)

// 帧切换配置：2秒彩条 + 2秒纯色
#define BAR_FRAME_DURATION 2
#define SOLID_FRAME_DURATION 2
#define BAR_FRAME_COUNT (FPS * BAR_FRAME_DURATION)
#define SOLID_FRAME_COUNT (FPS * SOLID_FRAME_DURATION)

typedef enum {
	SOLID_RED,
	SOLID_GREEN,
	SOLID_BLUE,
	SOLID_WHITE,
	SOLID_BLACK
} SolidColorType;

#define ERR_EXIT(msg) do { \
	fprintf(stderr, "%s: %s\n", msg, strerror(errno)); \
	exit(EXIT_FAILURE); \
} while (0)

typedef struct {
	void *start;
	size_t length;
} Buffer;

Buffer *buffers;
int dev_fd;
int frame_counter = 0;
int current_mode = 0;
SolidColorType current_color = SOLID_RED;

// 初始化视频输出（增加驱动兼容性）
void init_video_output() {
	struct v4l2_capability cap;
	struct v4l2_format fmt;
	struct v4l2_requestbuffers req;

	// 1. 打开设备：增加O_SYNC，强制同步写
	dev_fd = open(VIDEO_DEVICE, O_RDWR | O_NONBLOCK | O_SYNC);
	if (dev_fd < 0)
		ERR_EXIT("open video device");

	if (ioctl(dev_fd, VIDIOC_QUERYCAP, &cap) < 0)
		ERR_EXIT("VIDIOC_QUERYCAP");

	if (!(cap.capabilities & V4L2_CAP_VIDEO_OUTPUT) || !(cap.capabilities & V4L2_CAP_STREAMING))
		ERR_EXIT("device not support output/streaming");

	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	fmt.fmt.pix.width = WIDTH;
	fmt.fmt.pix.height = HEIGHT;
	fmt.fmt.pix.pixelformat = PIX_FMT;
	fmt.fmt.pix.field = V4L2_FIELD_NONE;
	fmt.fmt.pix.bytesperline = WIDTH * 2;
	fmt.fmt.pix.sizeimage = YUYV_FRAME_SIZE;
	// 修复：旧版驱动兼容，设为0（等价于无标志）
	fmt.fmt.pix.flags = 0;

	if (ioctl(dev_fd, VIDIOC_S_FMT, &fmt) < 0)
		ERR_EXIT("VIDIOC_S_FMT (YUYV)");

	printf("✅ 输出格式：%dx%d YUYV，帧率：%d FPS\n", WIDTH, HEIGHT, FPS);

	memset(&req, 0, sizeof(req));
	req.count = BUFFER_COUNT;
	req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	req.memory = V4L2_MEMORY_MMAP;

	if (ioctl(dev_fd, VIDIOC_REQBUFS, &req) < 0)
		ERR_EXIT("VIDIOC_REQBUFS");

	buffers = calloc(req.count, sizeof(Buffer));
	if (!buffers)
		ERR_EXIT("calloc buffers");

	for (int i = 0; i < req.count; i++) {
		struct v4l2_buffer buf;
		memset(&buf, 0, sizeof(buf));
		buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;

		if (ioctl(dev_fd, VIDIOC_QUERYBUF, &buf) < 0)
			ERR_EXIT("VIDIOC_QUERYBUF");

		buffers[i].length = buf.length;
		buffers[i].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, dev_fd, buf.m.offset);
		if (buffers[i].start == MAP_FAILED)
			ERR_EXIT("mmap");

		memset(buffers[i].start, 0, buf.length);
		if (ioctl(dev_fd, VIDIOC_QBUF, &buf) < 0)
			ERR_EXIT("VIDIOC_QBUF");
	}

	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	if (ioctl(dev_fd, VIDIOC_STREAMON, &type) < 0)
		ERR_EXIT("VIDIOC_STREAMON");

	printf("✅ 开始循环播放：%ds彩条 → %ds纯色 → 循环\n", BAR_FRAME_DURATION, SOLID_FRAME_DURATION);
}

// 生成75%标准彩条
void generate_75_color_bar(void *buf) {
	unsigned char *data = (unsigned char *)buf;
	unsigned char bars[8][4] = {
		{219, 128, 219, 128}, // 白
		{210, 16, 210, 240},  // 黄
		{170, 240, 170, 16},  // 青
		{161, 128, 161, 128}, // 绿
		{138, 16, 138, 240},  // 品红
		{129, 128, 129, 128}, // 红
		{89, 240, 89, 16},    // 蓝
		{0, 128, 0, 128}      // 黑
	};

	int bar_width = WIDTH / 8;
	for (int y = 0; y < HEIGHT; y++) {
		for (int x = 0; x < WIDTH; x += 2) {
			int idx = (y * WIDTH + x) * 2;
			if (idx + 3 >= YUYV_FRAME_SIZE) break;

			int bar_idx = x / bar_width;
			if (bar_idx >= 8) bar_idx = 7;

			memcpy(&data[idx], bars[bar_idx], 4);
		}
	}
}

// 生成纯色帧
void generate_solid_color(void *buf, SolidColorType color) {
	unsigned char *data = (unsigned char *)buf;
	unsigned char yuyv[4];

	switch (color) {
		case SOLID_RED:    yuyv[0]=76; yuyv[1]=85; yuyv[2]=76; yuyv[3]=255; break;
		case SOLID_GREEN:  yuyv[0]=88; yuyv[1]=0;  yuyv[2]=88; yuyv[3]=85; break;
		case SOLID_BLUE:   yuyv[0]=32; yuyv[1]=255;yuyv[2]=32; yuyv[3]=170; break;
		case SOLID_WHITE:  yuyv[0]=255;yuyv[1]=128;yuyv[2]=255;yuyv[3]=128; break;
		case SOLID_BLACK:  yuyv[0]=0;  yuyv[1]=128;yuyv[2]=0;  yuyv[3]=128; break;
		default:           yuyv[0]=128;yuyv[1]=128;yuyv[2]=128;yuyv[3]=128; break;
	}

	for (int y = 0; y < HEIGHT; y++) {
		for (int x = 0; x < WIDTH; x += 2) {
			int idx = (y * WIDTH + x) * 2;
			if (idx + 3 >= YUYV_FRAME_SIZE) break;
			memcpy(&data[idx], yuyv, 4);
		}
	}
}

// 帧切换逻辑
void switch_frame_mode(void *buf) {
	if (current_mode == 0) {
		generate_75_color_bar(buf);
		frame_counter++;
		if (frame_counter >= BAR_FRAME_COUNT) {
			frame_counter = 0;
			current_mode = 1;
			current_color = (SolidColorType)((current_color + 1) % 5);
			printf("\n🔄 切换为纯色帧：");
			switch (current_color) {
				case SOLID_RED:    printf("红色\n"); break;
				case SOLID_GREEN:  printf("绿色\n"); break;
				case SOLID_BLUE:   printf("蓝色\n"); break;
				case SOLID_WHITE:  printf("白色\n"); break;
				case SOLID_BLACK:  printf("黑色\n"); break;
			}
		}
	} else {
		generate_solid_color(buf, current_color);
		frame_counter++;
		if (frame_counter >= SOLID_FRAME_COUNT) {
			frame_counter = 0;
			current_mode = 0;
			printf("\n🔄 切换为75%标准彩条帧\n");
		}
	}
}

// 安全清理资源
void cleanup() {
	if (dev_fd > 0) {
		enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
		ioctl(dev_fd, VIDIOC_STREAMOFF, &type);
	}

	if (buffers) {
		for (int i = 0; i < BUFFER_COUNT; i++) {
			if (buffers[i].start) munmap(buffers[i].start, buffers[i].length);
		}
		free(buffers);
	}

	if (dev_fd > 0) close(dev_fd);
	printf("\n✅ 写端资源已清理\n");
}

int main() {
	int frame_interval = 1000000 / FPS;
	struct v4l2_buffer buf;

	atexit(cleanup);
	init_video_output();

	while (1) {
		struct timeval start, end;
		gettimeofday(&start, NULL);

		memset(&buf, 0, sizeof(buf));
		buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
		buf.memory = V4L2_MEMORY_MMAP;

		// 关键：增加重试次数，避免缓冲区空
		int retry = 3;
		while (retry-- > 0) {
			if (ioctl(dev_fd, VIDIOC_DQBUF, &buf) == 0) break;
			if (errno == EAGAIN) { usleep(5000); continue; }
			ERR_EXIT("VIDIOC_DQBUF");
		}
		if (retry < 0) {
			fprintf(stderr, "⚠️  缓冲区空，跳过当前帧\n");
			continue;
		}

		// 生成帧数据（先清空缓冲区，避免脏数据）
		memset(buffers[buf.index].start, 0, YUYV_FRAME_SIZE);
		switch_frame_mode(buffers[buf.index].start);

		// 重新入队前，标记缓冲区数据长度
		buf.bytesused = YUYV_FRAME_SIZE;
		if (ioctl(dev_fd, VIDIOC_QBUF, &buf) < 0)
			ERR_EXIT("VIDIOC_QBUF");

		// 严格控制帧率，避免驱动过载
		gettimeofday(&end, NULL);
		long elapsed = (end.tv_sec - start.tv_sec)*1000000 + (end.tv_usec - start.tv_usec);
		if (elapsed < frame_interval) {
			usleep(frame_interval - elapsed);
		}
	}

	return 0;
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <linux/videodev2.h>

#define BUFFER_COUNT 2
#define DEFAULT_DEVICE "/dev/video1"
#define DEFAULT_OUTPUT "yuyv_cycle_fix.yuv"
#define WIDTH 640
#define HEIGHT 480
#define YUYV_FRAME_SIZE (WIDTH * HEIGHT * 2)
#define FPS 15  // 和写端帧率一致

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
int out_fd;
volatile int stop = 0;

void sigint_handler(int sig) {
	stop = 1;
	printf("\n⚠️  接收到停止信号，正在停止捕获...\n");
}

void init_video_capture(const char *dev_path) {
	struct v4l2_capability cap;
	struct v4l2_format fmt;
	struct v4l2_requestbuffers req;

	// 打开设备：增加O_SYNC同步
	dev_fd = open(dev_path, O_RDWR | O_NONBLOCK | O_SYNC);
	if (dev_fd < 0)
		ERR_EXIT("open video device");

	if (ioctl(dev_fd, VIDIOC_QUERYCAP, &cap) < 0)
		ERR_EXIT("VIDIOC_QUERYCAP");

	if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) || !(cap.capabilities & V4L2_CAP_STREAMING))
		ERR_EXIT("device not support capture/streaming");

	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.width = WIDTH;
	fmt.fmt.pix.height = HEIGHT;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
	fmt.fmt.pix.field = V4L2_FIELD_NONE;
	fmt.fmt.pix.bytesperline = WIDTH * 2;
	fmt.fmt.pix.sizeimage = YUYV_FRAME_SIZE;
	// 修复：旧版驱动兼容，设为0
	fmt.fmt.pix.flags = 0;

	if (ioctl(dev_fd, VIDIOC_S_FMT, &fmt) < 0)
		ERR_EXIT("VIDIOC_S_FMT (YUYV)");

	printf("✅ 捕获格式配置成功：\n");
	printf("  分辨率：%dx%d\n", WIDTH, HEIGHT);
	printf("  格式：YUYV (YUV422)\n");
	printf("  帧率：%d FPS\n", FPS);

	memset(&req, 0, sizeof(req));
	req.count = BUFFER_COUNT;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_MMAP;

	if (ioctl(dev_fd, VIDIOC_REQBUFS, &req) < 0)
		ERR_EXIT("VIDIOC_REQBUFS");

	printf("✅ 捕获缓冲区数量：%d\n", req.count);

	buffers = calloc(req.count, sizeof(Buffer));
	if (!buffers)
		ERR_EXIT("calloc buffers");

	for (int i = 0; i < req.count; i++) {
		struct v4l2_buffer buf;
		memset(&buf, 0, sizeof(buf));
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;

		if (ioctl(dev_fd, VIDIOC_QUERYBUF, &buf) < 0)
			ERR_EXIT("VIDIOC_QUERYBUF");

		buffers[i].length = buf.length;
		buffers[i].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, dev_fd, buf.m.offset);
		if (buffers[i].start == MAP_FAILED)
			ERR_EXIT("mmap");

		if (ioctl(dev_fd, VIDIOC_QBUF, &buf) < 0)
			ERR_EXIT("VIDIOC_QBUF");
	}

	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (ioctl(dev_fd, VIDIOC_STREAMON, &type) < 0)
		ERR_EXIT("VIDIOC_STREAMON");

	printf("✅ 捕获流已启动（按Ctrl+C停止）\n");
}

void cleanup() {
	if (dev_fd > 0) {
		enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		ioctl(dev_fd, VIDIOC_STREAMOFF, &type);
	}

	if (buffers) {
		for (int i = 0; i < BUFFER_COUNT; i++) {
			if (buffers[i].start) munmap(buffers[i].start, buffers[i].length);
		}
		free(buffers);
	}

	if (out_fd > 0) close(out_fd);
	if (dev_fd > 0) close(dev_fd);
	printf("✅ 捕获资源已清理\n");
}

int main(int argc, char *argv[]) {
	char *dev_path = DEFAULT_DEVICE;
	char *out_path = DEFAULT_OUTPUT;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-d") == 0 && i+1 < argc) dev_path = argv[++i];
		else if (strcmp(argv[i], "-o") == 0 && i+1 < argc) out_path = argv[++i];
	}

	signal(SIGINT, sigint_handler);
	atexit(cleanup);

	init_video_capture(dev_path);

	// 打开文件：增加O_SYNC，确保写入完成
	out_fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC | O_SYNC, 0644);
	if (out_fd < 0)
		ERR_EXIT("open output file");

	printf("📹 开始捕获，输出文件：%s\n", out_path);

	int frame_count = 0;
	while (!stop) {
		fd_set fds;
		struct timeval tv = {10, 0}; // 延长超时到10秒，避免误判
		FD_ZERO(&fds);
		FD_SET(dev_fd, &fds);

		int ret = select(dev_fd + 1, &fds, NULL, NULL, &tv);
		if (ret < 0) {
			if (errno == EINTR) continue;
			ERR_EXIT("select");
		} else if (ret == 0) {
			fprintf(stderr, "\r⚠️  捕获超时，跳过当前帧\n");
			fflush(stderr);
			continue;
		}

		struct v4l2_buffer buf;
		memset(&buf, 0, sizeof(buf));
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;

		// 增加DQBUF重试
		int retry = 3;
		while (retry-- > 0) {
			if (ioctl(dev_fd, VIDIOC_DQBUF, &buf) == 0) break;
			if (errno == EAGAIN) { usleep(5000); continue; }
			ERR_EXIT("VIDIOC_DQBUF");
		}
		if (retry < 0) {
			fprintf(stderr, "\r⚠️  缓冲区空，跳过帧\n");
			fflush(stderr);
			continue;
		}

		// 关键修复：按驱动返回的实际可用字节写入，不强制614400
		ssize_t written = 0;
		if (buf.bytesused > 0 && buf.bytesused <= YUYV_FRAME_SIZE) {
			written = write(out_fd, buffers[buf.index].start, buf.bytesused);
		}

		if (written > 0) {
			frame_count++;
			printf("\r✅ 已捕获 %d 帧（本次写入：%zd 字节）", frame_count, written);
			fflush(stdout);
		} else {
			fprintf(stderr, "\r❌ 帧写入失败（错误码：%d）", errno);
			fflush(stderr);
		}

		// 重新入队前，重置缓冲区标记
		buf.bytesused = 0;
		if (ioctl(dev_fd, VIDIOC_QBUF, &buf) < 0)
			ERR_EXIT("VIDIOC_QBUF");
	}

	printf("\n📊 捕获完成：共捕获 %d 帧YUYV数据，文件：%s\n", frame_count, out_path);
	return 0;
}
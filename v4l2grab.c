#include <stdio.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/time.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/syscall.h>

#define FORMAT V4L2_PIX_FMT_MJPEG
#define WIDTH 640
#define HEIGHT 480

struct Buffer
{
	void *start;
	ssize_t length;
};

long xioctl(int fd, unsigned long request, void *arg)
{
	long r;
	while (1)
	{
		r = syscall(SYS_ioctl, fd, request, arg);
		if (r == -1 && (errno == EINTR || errno == EAGAIN))
		{
			continue;
		}
		else
		{
			break;
		}
	}
	return r;
}

static int buf_ioctl(int fd, unsigned long int cmd, struct v4l2_buffer *arg)
{
	struct v4l2_buffer buf = *arg;
	struct v4l2_plane plane = {0};
	int ret;

	memcpy(&plane.m, &arg->m, sizeof(plane.m));
	plane.length = arg->length;
	plane.bytesused = arg->bytesused;

	buf.m.planes = &plane;
	buf.length = 1;

	ret = xioctl(fd, cmd, &buf);

	arg->index = buf.index;
	arg->memory = buf.memory;
	arg->flags = buf.flags;
	arg->field = buf.field;
	arg->timestamp = buf.timestamp;
	arg->timecode = buf.timecode;
	arg->sequence = buf.sequence;

	arg->length = plane.length;
	arg->bytesused = plane.bytesused;
	memcpy(&arg->m, &plane.m, sizeof(arg->m));

	return ret;
}

int main()
{
	printf("Starting v4l2grab...\n");

	int fd = open("/dev/video0", O_RDWR | O_NONBLOCK, 0);
	if (fd < 0)
	{
		printf("ERROR: Cannot open device\n");
		return EXIT_FAILURE;
	}

	struct v4l2_format vid_format;
	vid_format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	vid_format.fmt.pix_mp.width = WIDTH;
	vid_format.fmt.pix_mp.height = HEIGHT;
	vid_format.fmt.pix_mp.pixelformat = FORMAT;
	vid_format.fmt.pix_mp.field = V4L2_FIELD_INTERLACED;

	xioctl(fd, VIDIOC_S_FMT, &vid_format);

	if (vid_format.fmt.pix_mp.pixelformat != FORMAT)
	{
		printf("Didn't accept other format. \n");
		close(fd);
		return EXIT_FAILURE;
	}

	if ((vid_format.fmt.pix_mp.width != WIDTH) || (vid_format.fmt.pix_mp.height != HEIGHT))
	{
		printf("Warning: Driver is sending image at %dx%d", vid_format.fmt.pix_mp.width, vid_format.fmt.pix_mp.height);
	}

	struct v4l2_requestbuffers req;
	req.count = 1;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	req.memory = V4L2_MEMORY_MMAP;

	xioctl(fd, VIDIOC_REQBUFS, &req);

	struct Buffer *buffers = malloc(req.count * sizeof(struct Buffer));
	for (int i = 0; i < req.count; i++)
	{
		struct v4l2_buffer buff;
		struct v4l2_plane plane = {0};

		buff.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		buff.memory = V4L2_MEMORY_MMAP;
		buff.index = i;
		buf_ioctl(fd, VIDIOC_QUERYBUF, &buff);

		struct Buffer buffer;
		buffer.start = (void *)syscall(SYS_mmap, NULL, (size_t)buff.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buff.m.offset);
		buffer.length = buff.length;
		if (MAP_FAILED == buffer.start)
		{
			printf("Failed mmap\n");
			perror("mmap");
			close(fd);
			free(buffers);
			return EXIT_FAILURE;
		}
		buffers[i] = buffer;
	}

	for (int i = 0; i < req.count; i++)
	{
		struct v4l2_buffer buff;
		buff.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		buff.memory = V4L2_MEMORY_MMAP;
		buff.index = i;
		buf_ioctl(fd, VIDIOC_QBUF, &buff);
	}
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	xioctl(fd, VIDIOC_STREAMON, &type);

	fd_set fds;
	FD_ZERO(&fds);
	FD_SET(fd, &fds);
	struct timeval tv;
	tv.tv_sec = 2;
	tv.tv_usec = 0;

	int r = select(fd + 1, &fds, NULL, NULL, &tv);
	if (!(r == -1))
	{
		if (__linux__)
		{
			printf("Time left: %ld.%06ld\n", tv.tv_sec, tv.tv_usec);
		}
	}
	else if (r == -1)
	{
		printf("Error\n");
		close(fd);
		free(buffers);
		return EXIT_FAILURE;
	}

	struct v4l2_buffer buf;
	buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	buf.memory = V4L2_MEMORY_MMAP;

	buf_ioctl(fd, VIDIOC_DQBUF, &buf);

	FILE *fout = fopen("out.ppm", "wb");
	if (fout == NULL)
	{
		printf("Failed to open output file\n");
		close(fd);
		free(buffers);
		return EXIT_FAILURE;
	}

	u_int8_t *vdata = malloc(buf.bytesused);
	if (vdata == NULL)
	{
		printf("Memory allocation failed for vdata\n");
		fclose(fout);
		close(fd);
		free(buffers);
		return EXIT_FAILURE;
	}

	ssize_t dlen_size = buf.bytesused;
	memcpy(vdata, buffers[buf.index].start, dlen_size);

	fwrite(vdata, sizeof(u_int8_t), dlen_size, fout);

	free(vdata);
	fclose(fout);
	buf_ioctl(fd, VIDIOC_QBUF, &buf);
	enum v4l2_buf_type type_off = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	xioctl(fd, VIDIOC_STREAMOFF, &type_off);

	for (int i = 0; i < req.count; i++)
	{
		munmap(buffers[i].start, buffers[i].length);
	}

	close(fd);
	free(buffers);

	return 0;
}

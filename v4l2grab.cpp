#undef CPP
#ifdef CPP

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#else
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#endif

#include <fcntl.h>
#include <unistd.h>

#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#define FORMAT V4L2_PIX_FMT_RGB24
#define WIDTH 640
#define HEIGHT 480
#define SOF 0xEAFF99DEADFF
#define EOP 0xEAFF99DEADAA
#define FRAME_WIDTH 48

struct FPGAFrame {
  u_int64_t sof;
  u_int8_t appId;
  u_int32_t data_length;
  u_int8_t mask;
  u_int8_t reserved;
  u_int64_t *data;
  u_int64_t eof;
};

void display_fpga_frame(struct FPGAFrame fpga_frame) {
  printf("SOF              :%lx\n", fpga_frame.sof);
  printf("App Id           :%lx\n", fpga_frame.appId);
  printf("Data Length      :%lx\n", fpga_frame.data_length);
  printf("Mask             :%lx\n", fpga_frame.mask);
  printf("Reserved         :%lx\n", fpga_frame.reserved);
  for (int i = 0; i < fpga_frame.data_length; i++) {
    printf("Data-%d           :%lx\n", i, fpga_frame.data[i]);
  }
  printf("EOF              :%lx\n", fpga_frame.eof);
}

ssize_t checkSOF(ssize_t start_idx, u_int8_t *vdata, ssize_t dlen_size) {
  for (ssize_t idx = start_idx; idx < dlen_size - 6; idx++) {
    if (vdata[idx] != 0xEA) {
      continue;
    }

    u_int8_t temp[] = {vdata[idx + 5], vdata[idx + 4], vdata[idx + 3],
                       vdata[idx + 2], vdata[idx + 1], vdata[idx]};
    u_int64_t sof_chk = 0;
    memcpy(&sof_chk, temp, 6);
    if (sof_chk == 0xEAFF99DEADFF) {
      return idx;
    }
  }
  return -1;
}

struct FPGAFrame populate_frame(ssize_t idx, u_int8_t *vdata) {

  struct FPGAFrame fpga_frame;

  u_int8_t sof_arr[] = {vdata[idx + 5], vdata[idx + 4], vdata[idx + 3],
                        vdata[idx + 2], vdata[idx + 1], vdata[idx]};

  u_int64_t sof_tmp = 0;
  memcpy(&sof_tmp, sof_arr, 6);
  fpga_frame.sof = sof_tmp;

  fpga_frame.appId = vdata[idx + 6];

  u_int8_t dl_arr[] = {vdata[idx + 9], vdata[idx + 8], vdata[idx + 7]};
  u_int64_t dl_tmp = 0;
  memcpy(&dl_tmp, dl_arr, 3);
  fpga_frame.data_length = dl_tmp;

  fpga_frame.mask = vdata[idx + 10];

  fpga_frame.reserved = vdata[idx + 11];

#ifdef CPP
  fpga_frame.data = new u_int64_t[fpga_frame.data_length * sizeof(u_int64_t)];
#else
  fpga_frame.data =
      (u_int64_t *)malloc(fpga_frame.data_length * sizeof(u_int64_t));
#endif
  int data_itr = idx;
  for (int i = 0; i < fpga_frame.data_length; i++) {
    u_int8_t data_arr[] = {vdata[data_itr + 17], vdata[data_itr + 16],
                           vdata[data_itr + 15], vdata[data_itr + 14],
                           vdata[data_itr + 13], vdata[data_itr + 12]};
    u_int64_t data_tmp = 0;
    memcpy(&data_tmp, data_arr, 6);
    fpga_frame.data[i] = data_tmp;
    data_itr += 12 + fpga_frame.data_length + 1;
  }
  u_int8_t eof_arr[] = {vdata[data_itr + 9], vdata[data_itr + 8],
                        vdata[data_itr + 7], vdata[data_itr + 6],
                        vdata[data_itr + 5], vdata[data_itr + 4]};

  u_int64_t eof_tmp = 0;
  memcpy(&eof_tmp, eof_arr, 6);
  fpga_frame.eof = eof_tmp;

  return fpga_frame;
}

struct Buffer {
  void *start;
  ssize_t length;
};

long xioctl(int fd, unsigned long request, void *arg) {
  long r;
  while (1) {
    r = ioctl(fd, request, arg);
    if (r == -1 && (errno == EINTR || errno == EAGAIN)) {
      continue;
    } else {
      break;
    }
  }
  return r;
}

static int buf_ioctl(int fd, unsigned long int cmd, struct v4l2_buffer *arg) {
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

int main() {
  int r;
#ifdef CPP
  std::cout << "Starting v4l2grab..." << std::endl;
#else
  printf("Stating v4l2grab...\n");
#endif

  int fd = open("/dev/video5", O_RDWR | O_NONBLOCK, 0);
  if (fd < 0) {
    perror("video dev");
    exit(EXIT_FAILURE);
  }

  struct v4l2_format vid_format;
  vid_format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  vid_format.fmt.pix_mp.width = WIDTH;
  vid_format.fmt.pix_mp.height = HEIGHT;
  vid_format.fmt.pix_mp.pixelformat = FORMAT;
  vid_format.fmt.pix_mp.field = V4L2_FIELD_INTERLACED;

  xioctl(fd, VIDIOC_S_FMT, &vid_format);

  if (vid_format.fmt.pix_mp.pixelformat != FORMAT) {
#ifdef CPP
    std::cout << "Didn't accept other format." << std::endl;
#else
    printf("Didn't accept other format\n");
#endif
    close(fd);
    exit(EXIT_FAILURE);
  }

  if ((vid_format.fmt.pix_mp.width != WIDTH) ||
      (vid_format.fmt.pix_mp.height != HEIGHT)) {
#ifdef CPP
    std::cout << "Warning: Driver is sending image at "
              << vid_format.fmt.pix_mp.width << "x"
              << vid_format.fmt.pix_mp.height << std::endl;
#else
    printf("Warning: Driver is sending image at %dx%d\n",
           vid_format.fmt.pix_mp.width, vid_format.fmt.pix_mp.height);
#endif
  }

  struct v4l2_requestbuffers req;
  req.count = 60;
  req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  req.memory = V4L2_MEMORY_MMAP;

  r = xioctl(fd, VIDIOC_REQBUFS, &req);
#ifdef CPP
  std::cout << "Request Count: " << req.count << std::endl;

  struct Buffer *buffers = new struct Buffer[req.count];
#else
  printf("Request Count: %d\n", req.count);

  struct Buffer *buffers =
      (struct Buffer *)malloc(sizeof(struct Buffer) * req.count);
#endif
  for (int i = 0; i < req.count; i++) {
    struct v4l2_buffer buff;
    struct v4l2_plane plane = {0};

    buff.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buff.memory = V4L2_MEMORY_MMAP;
    buff.index = i;
    r = buf_ioctl(fd, VIDIOC_QUERYBUF, &buff);
    struct Buffer buffer;
    buffer.start =
        (void *)mmap(NULL, (size_t)buff.length, PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd, buff.m.offset);
    buffer.length = buff.length;
    if (MAP_FAILED == buffer.start) {
      perror("mmap");
      close(fd);
#ifdef CPP
      delete[] buffers;
#else
      free(buffers);
#endif
      exit(EXIT_FAILURE);
    }
    buffers[i] = buffer;
  }

  for (int i = 0; i < req.count; i++) {
    struct v4l2_buffer buff;
    buff.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buff.memory = V4L2_MEMORY_MMAP;
    buff.index = i;
    r = buf_ioctl(fd, VIDIOC_QBUF, &buff);
  }

  enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  r = xioctl(fd, VIDIOC_STREAMON, &type);
  fd_set fds;
  FD_ZERO(&fds);
  FD_SET(fd, &fds);

  r = select(fd + 1, &fds, NULL, NULL, NULL);
  if (r == -1) {
    perror("select");
    close(fd);
#ifdef CPP
    delete[] buffers;
#else
    free(buffers);
#endif
    exit(EXIT_FAILURE);
  }

  struct v4l2_buffer buf;
  buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  buf.memory = V4L2_MEMORY_MMAP;

  r = buf_ioctl(fd, VIDIOC_DQBUF, &buf);
  FILE *fout = fopen("out.ppm", "wb");
  if (fout == NULL) {
    perror("out.ppm");
    close(fd);
#ifdef CPP
    delete[] buffers;
#else
    free(buffers);
#endif
    exit(EXIT_FAILURE);
  }

#ifdef CPP
  u_int8_t *vdata = new u_int8_t[buf.bytesused * req.count];
#else
  u_int8_t *vdata = (u_int8_t *)malloc(buf.bytesused * req.count);
#endif

  if (vdata == NULL) {
    perror("Memory allocation failed for vdata");
    fclose(fout);
    close(fd);
#ifdef CPP
    delete[] buffers;
#else
    free(buffers);
#endif
    exit(EXIT_FAILURE);
  }

  ssize_t dlen_size = buf.bytesused;

  for (int i = 0; i < req.count; i++) {
    memcpy(vdata + (dlen_size * i), buffers[i].start, dlen_size);
  }
  fwrite(vdata, sizeof(u_int8_t), dlen_size, fout);

  printf("It is: %d\n", dlen_size);
  ssize_t index = -1;

  while (index - 6 < dlen_size) {
    struct FPGAFrame fpga_frame;
    printf("Index: %ld\n", index);
    index = checkSOF(index + 1, vdata, dlen_size);
    if (index != -1) {
      fpga_frame = populate_frame(index, vdata);
      display_fpga_frame(fpga_frame);
    } else {
      printf("Wrong data\n");
      break;
    }
  }

#ifdef CPP
  delete[] vdata;
#else
  free(vdata);
#endif
  fclose(fout);
  r = buf_ioctl(fd, VIDIOC_QBUF, &buf);
  enum v4l2_buf_type type_off = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  r = xioctl(fd, VIDIOC_STREAMOFF, &type_off);
  for (int i = 0; i < req.count; i++) {
    munmap(buffers[i].start, buffers[i].length);
  }

  close(fd);
#ifdef CPP
  delete[] buffers;
#else
  free(buffers);
#endif
  printf("Success!\n");
  exit(EXIT_SUCCESS);
}
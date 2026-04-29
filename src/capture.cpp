#include "capture.h"
#include "common.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <unistd.h>
#include <linux/videodev2.h>

static int xioctl(int fd, int req, void* arg) {
    int r;
    do {
        r = ioctl(fd, req, arg);
    } while (r == -1 && errno == EINTR);
    return r;
}

void capture_thread(RingQueue<RawFrame, 4>& queue, std::atomic<bool>& running) {
    const char* dev = "/dev/video0";
    const int W = FRAME_WIDTH;
    const int H = FRAME_HEIGHT;
    const int NBUF = 4;

    int fd = open(dev, O_RDWR);
    if (fd < 0) {
        perror("[capture] open /dev/video0");
        return;
    }

    v4l2_format fmt{};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.width = W;
    fmt.fmt.pix_mp.height = H;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
    fmt.fmt.pix_mp.num_planes = 1;

    if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
        perror("[capture] VIDIOC_S_FMT NV12 mplane");
        close(fd);
        return;
    }

    printf("[capture] format set: /dev/video0 %dx%d NV12 MPLANE\n", W, H);

    v4l2_requestbuffers req{};
    req.count = NBUF;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;

    if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
        perror("[capture] VIDIOC_REQBUFS");
        close(fd);
        return;
    }

    struct Buffer {
        void* start = nullptr;
        size_t len = 0;
    };

    Buffer bufs[NBUF];

    for (int i = 0; i < NBUF; i++) {
        v4l2_buffer buf{};
        v4l2_plane planes[VIDEO_MAX_PLANES]{};

        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        buf.length = VIDEO_MAX_PLANES;
        buf.m.planes = planes;

        if (xioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
            perror("[capture] VIDIOC_QUERYBUF");
            close(fd);
            return;
        }

        bufs[i].len = buf.m.planes[0].length;
        bufs[i].start = mmap(nullptr, bufs[i].len,
                             PROT_READ | PROT_WRITE,
                             MAP_SHARED,
                             fd,
                             buf.m.planes[0].m.mem_offset);

        if (bufs[i].start == MAP_FAILED) {
            perror("[capture] mmap");
            close(fd);
            return;
        }

        if (xioctl(fd, VIDIOC_QBUF, &buf) < 0) {
            perror("[capture] VIDIOC_QBUF");
            close(fd);
            return;
        }
    }

    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (xioctl(fd, VIDIOC_STREAMON, &type) < 0) {
        perror("[capture] VIDIOC_STREAMON");
        close(fd);
        return;
    }

    printf("[capture] stream on\n");

    uint64_t seq = 0;

    while (running.load()) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);

        timeval tv{2, 0};
        int sel = select(fd + 1, &fds, nullptr, nullptr, &tv);
        if (sel <= 0) {
            continue;
        }

        v4l2_buffer buf{};
        v4l2_plane planes[VIDEO_MAX_PLANES]{};

        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.length = VIDEO_MAX_PLANES;
        buf.m.planes = planes;

        if (xioctl(fd, VIDIOC_DQBUF, &buf) < 0) {
            perror("[capture] VIDIOC_DQBUF");
            continue;
        }

        size_t bytesused = buf.m.planes[0].bytesused;
        if (bytesused > FRAME_NV12_SIZE) {
            bytesused = FRAME_NV12_SIZE;
        }

        RawFrame frame;
        frame.width = W;
        frame.height = H;
        frame.seq = seq++;

        memcpy(frame.data, bufs[buf.index].start, bytesused);

        if (!queue.push(frame)) {
            // queue full, drop this frame
        }

        if (xioctl(fd, VIDIOC_QBUF, &buf) < 0) {
            perror("[capture] VIDIOC_QBUF requeue");
            break;
        }
    }

    xioctl(fd, VIDIOC_STREAMOFF, &type);

    for (int i = 0; i < NBUF; i++) {
        if (bufs[i].start && bufs[i].start != MAP_FAILED) {
            munmap(bufs[i].start, bufs[i].len);
        }
    }

    close(fd);
    printf("[capture] stream off\n");
}

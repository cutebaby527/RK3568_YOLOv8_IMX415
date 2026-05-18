#include "inference.h"
#include "common.h"

#include <atomic>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include "yolov8.h"

/*
 * YOLOv8 推理线程版本：
 *
 * 输入：
 *   capture_thread 输出 640x480 NV12
 *
 * 处理：
 *   1. NV12 -> RGB888
 *   2. 调用 RKNN Model Zoo 的 inference_yolov8_model()
 *   3. YOLOv8 后处理得到 object_detect_result_list
 *   4. 写入共享内存，供 web_viewer 画框
 *   5. 推送 InferResult 到 alarm_thread，供 MQTT 发布
 *
 * 当前实现为了先跑通链路，NV12 -> RGB888 用 OpenCV CPU 转换。
 * 后续可优化成 RGA 硬件转换。
 */

static int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

void inference_thread(
    RingQueue<RawFrame, 4>& in_queue,
    RingQueue<InferResult, 4>& out_queue,
    std::atomic<bool>& running,
    const char* model_path
) {
    shm_unlink(SHM_NAME);

    int shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd < 0) {
        perror("[inference] shm_open");
        return;
    }

    if (ftruncate(shm_fd, sizeof(SharedFrame)) != 0) {
        perror("[inference] ftruncate");
        close(shm_fd);
        shm_unlink(SHM_NAME);
        return;
    }

    SharedFrame* shm = static_cast<SharedFrame*>(
        mmap(nullptr,
             sizeof(SharedFrame),
             PROT_READ | PROT_WRITE,
             MAP_SHARED,
             shm_fd,
             0)
    );

    if (shm == MAP_FAILED) {
        perror("[inference] mmap");
        close(shm_fd);
        shm_unlink(SHM_NAME);
        return;
    }

    new (shm) SharedFrame();

    printf("[inference] YOLOv8 RKNN mode\n");
    printf("[inference] model path: %s\n", model_path);
    printf("[inference] shared memory: %s\n", SHM_NAME);
    printf("[inference] camera frame: %dx%d NV12, bytes=%d\n",
           FRAME_WIDTH,
           FRAME_HEIGHT,
           FRAME_NV12_SIZE);

    int ret = 0;

    rknn_app_context_t rknn_app_ctx;
    memset(&rknn_app_ctx, 0, sizeof(rknn_app_context_t));

    ret = init_post_process();
    if (ret != 0) {
        printf("[inference] init_post_process failed! ret=%d\n", ret);
        munmap(shm, sizeof(SharedFrame));
        close(shm_fd);
        shm_unlink(SHM_NAME);
        return;
    }

    ret = init_yolov8_model(model_path, &rknn_app_ctx);
    if (ret != 0) {
        printf("[inference] init_yolov8_model failed! ret=%d model=%s\n",
               ret,
               model_path);
        deinit_post_process();
        munmap(shm, sizeof(SharedFrame));
        close(shm_fd);
        shm_unlink(SHM_NAME);
        return;
    }

    printf("[inference] YOLOv8 model initialized\n");
    printf("[inference] model input: %dx%dx%d\n",
           rknn_app_ctx.model_width,
           rknn_app_ctx.model_height,
           rknn_app_ctx.model_channel);

    RawFrame frame;
    uint64_t frame_index = 0;

    while (running.load(std::memory_order_acquire)) {
        if (!in_queue.pop(frame)) {
            usleep(1000);
            continue;
        }

        /*
         * 先把原始 NV12 帧写入共享内存。
         * web_viewer 使用这份 NV12 做视频显示。
         */
        memcpy(shm->nv12, frame.data, FRAME_NV12_SIZE);

        cv::Mat nv12_mat(
            FRAME_HEIGHT * 3 / 2,
            FRAME_WIDTH,
            CV_8UC1,
            frame.data
        );

        cv::Mat rgb_mat;

        try {
            cv::cvtColor(nv12_mat, rgb_mat, cv::COLOR_YUV2RGB_NV12);
        } catch (const cv::Exception& e) {
            fprintf(stderr, "[inference] cvtColor NV12->RGB failed: %s\n", e.what());
            shm->detect_count = 0;
            shm->seq.fetch_add(1, std::memory_order_release);
            continue;
        }

        if (!rgb_mat.isContinuous()) {
            rgb_mat = rgb_mat.clone();
        }

        image_buffer_t src_image;
        memset(&src_image, 0, sizeof(image_buffer_t));

        src_image.width = FRAME_WIDTH;
        src_image.height = FRAME_HEIGHT;
        src_image.width_stride = FRAME_WIDTH;
        src_image.height_stride = FRAME_HEIGHT;
        src_image.format = IMAGE_FORMAT_RGB888;
        src_image.virt_addr = rgb_mat.data;
        src_image.size = FRAME_WIDTH * FRAME_HEIGHT * 3;

        object_detect_result_list od_results;
        memset(&od_results, 0, sizeof(od_results));

        ret = inference_yolov8_model(&rknn_app_ctx, &src_image, &od_results);

        if (ret != 0) {
            fprintf(stderr, "[inference] inference_yolov8_model failed! ret=%d\n", ret);

            shm->detect_count = 0;
            shm->seq.fetch_add(1, std::memory_order_release);
            continue;
        }

        int count = od_results.count;
        if (count < 0) {
            count = 0;
        }
        if (count > 64) {
            count = 64;
        }

        shm->detect_count = count;

        InferResult infer_result;
        memset(&infer_result, 0, sizeof(infer_result));
        infer_result.seq = frame.seq;
        infer_result.count = count;

        for (int i = 0; i < count; ++i) {
            object_detect_result* det = &od_results.results[i];

            int left = clamp_int(det->box.left, 0, FRAME_WIDTH - 1);
            int top = clamp_int(det->box.top, 0, FRAME_HEIGHT - 1);
            int right = clamp_int(det->box.right, 0, FRAME_WIDTH - 1);
            int bottom = clamp_int(det->box.bottom, 0, FRAME_HEIGHT - 1);

            if (right <= left || bottom <= top) {
                continue;
            }

            const char* cls_name = coco_cls_to_name(det->cls_id);
            if (!cls_name) {
                cls_name = "unknown";
            }
            if (std::strcmp(cls_name, "person") == 0 && frame_index % 5 == 0) {
                const int cx = (left + right) / 2;
                const int cy = (top + bottom) / 2;

                std::printf(
                    "[det] seq=%llu class=%s conf=%.3f bbox=[%d,%d,%d,%d] center=[%d,%d]\n",
                    static_cast<unsigned long long>(frame.seq),
                    cls_name,
                    det->prop,
                    left,
                    top,
                    right,
                    bottom,
                    cx,
                    cy
                );
            }

            strncpy(shm->detects[i].name, cls_name, sizeof(shm->detects[i].name) - 1);
            shm->detects[i].name[sizeof(shm->detects[i].name) - 1] = '\0';
            shm->detects[i].prop = det->prop;
            shm->detects[i].left = left;
            shm->detects[i].top = top;
            shm->detects[i].right = right;
            shm->detects[i].bottom = bottom;

            strncpy(infer_result.results[i].name, cls_name, sizeof(infer_result.results[i].name) - 1);
            infer_result.results[i].name[sizeof(infer_result.results[i].name) - 1] = '\0';
            infer_result.results[i].prop = det->prop;
            infer_result.results[i].left = left;
            infer_result.results[i].top = top;
            infer_result.results[i].right = right;
            infer_result.results[i].bottom = bottom;
        }

        shm->seq.fetch_add(1, std::memory_order_release);

        /*
         * 推给 alarm_thread。
         * 如果队列满，直接丢弃，不阻塞实时视频。
         */
        out_queue.push(infer_result);

        frame_index++;

        if (frame_index % 25 == 0) {
            printf("[inference] frame=%llu detect_count=%d\n",
                   static_cast<unsigned long long>(frame_index),
                   count);
        }
    }

    printf("[inference] releasing YOLOv8 model\n");

    release_yolov8_model(&rknn_app_ctx);
    deinit_post_process();

    munmap(shm, sizeof(SharedFrame));
    close(shm_fd);
    shm_unlink(SHM_NAME);

    printf("[inference] exited\n");
}

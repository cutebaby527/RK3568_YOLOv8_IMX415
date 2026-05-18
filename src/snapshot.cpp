#include "snapshot.h"
#include "common.h"

#include <opencv2/opencv.hpp>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

static bool ensure_dir_exists(const std::string& dir, std::string& error) {
    if (dir.empty()) {
        error = "snapshot save_dir is empty";
        return false;
    }

    if (::mkdir(dir.c_str(), 0755) != 0) {
        if (errno != EEXIST) {
            error = "failed to create snapshot dir: " + dir + ", errno=" + std::to_string(errno);
            return false;
        }
    }

    return true;
}

static std::string join_path(const std::string& dir, const std::string& file) {
    if (dir.empty()) {
        return file;
    }

    if (dir.back() == '/') {
        return dir + file;
    }

    return dir + "/" + file;
}

static std::string current_time_string() {
    const std::time_t now = std::time(nullptr);
    struct tm tm_buf {};
    localtime_r(&now, &tm_buf);

    char buf[64];
    std::snprintf(
        buf,
        sizeof(buf),
        "%04d-%02d-%02d %02d:%02d:%02d",
        tm_buf.tm_year + 1900,
        tm_buf.tm_mon + 1,
        tm_buf.tm_mday,
        tm_buf.tm_hour,
        tm_buf.tm_min,
        tm_buf.tm_sec
    );

    return std::string(buf);
}

static void draw_roi(cv::Mat& image, const std::vector<RoiPoint>& roi) {
    if (roi.size() < 3) {
        return;
    }

    std::vector<cv::Point> points;
    points.reserve(roi.size());

    for (const auto& p : roi) {
        points.emplace_back(p.x, p.y);
    }

    const cv::Scalar roi_color(0, 255, 255);
    cv::polylines(image, points, true, roi_color, 2, cv::LINE_AA);

    for (const auto& p : roi) {
        cv::circle(image, cv::Point(p.x, p.y), 4, roi_color, -1, cv::LINE_AA);
    }
}

static void draw_detection(
    cv::Mat& image,
    const std::string& event_id,
    const RuleDetection& detection
) {
    const cv::Scalar box_color(0, 255, 0);
    const cv::Scalar text_color(255, 255, 255);
    const cv::Scalar bg_color(0, 0, 0);

    cv::rectangle(
        image,
        cv::Point(detection.x1, detection.y1),
        cv::Point(detection.x2, detection.y2),
        box_color,
        2,
        cv::LINE_AA
    );

    char label[128];
    std::snprintf(
        label,
        sizeof(label),
        "%s %.3f",
        detection.class_name.c_str(),
        detection.confidence
    );

    int baseline = 0;
    cv::Size label_size = cv::getTextSize(
        label,
        cv::FONT_HERSHEY_SIMPLEX,
        0.5,
        1,
        &baseline
    );

    int label_x = detection.x1;
    int label_y = detection.y1 - 8;
    if (label_y < label_size.height + 4) {
        label_y = detection.y1 + label_size.height + 8;
    }

    cv::rectangle(
        image,
        cv::Point(label_x, label_y - label_size.height - 4),
        cv::Point(label_x + label_size.width + 6, label_y + baseline),
        bg_color,
        -1
    );

    cv::putText(
        image,
        label,
        cv::Point(label_x + 3, label_y - 2),
        cv::FONT_HERSHEY_SIMPLEX,
        0.5,
        text_color,
        1,
        cv::LINE_AA
    );

    const std::string line1 = "event_id: " + event_id;
    const std::string line2 = "time: " + current_time_string();

    cv::rectangle(
        image,
        cv::Point(8, 8),
        cv::Point(620, 62),
        bg_color,
        -1
    );

    cv::putText(
        image,
        line1,
        cv::Point(16, 30),
        cv::FONT_HERSHEY_SIMPLEX,
        0.55,
        text_color,
        1,
        cv::LINE_AA
    );

    cv::putText(
        image,
        line2,
        cv::Point(16, 54),
        cv::FONT_HERSHEY_SIMPLEX,
        0.55,
        text_color,
        1,
        cv::LINE_AA
    );
}

bool save_alarm_snapshot(
    const std::string& event_id,
    const RuleDetection& detection,
    const std::vector<RoiPoint>& roi,
    const SnapshotConfig& snapshot_config,
    std::string& snapshot_path,
    std::string& error
) {
    snapshot_path.clear();
    error.clear();

    if (!snapshot_config.enable) {
        return true;
    }

    if (!ensure_dir_exists(snapshot_config.save_dir, error)) {
        return false;
    }

    int fd = ::shm_open(SHM_NAME, O_RDONLY, 0666);
    if (fd < 0) {
        error = "failed to open shared memory: " + std::string(SHM_NAME) +
                ", errno=" + std::to_string(errno);
        return false;
    }

    void* ptr = ::mmap(
        nullptr,
        sizeof(SharedFrame),
        PROT_READ,
        MAP_SHARED,
        fd,
        0
    );

    if (ptr == MAP_FAILED) {
        error = "failed to mmap shared memory, errno=" + std::to_string(errno);
        ::close(fd);
        return false;
    }

    const auto* shared = static_cast<const SharedFrame*>(ptr);

    std::vector<uint8_t> nv12(FRAME_NV12_SIZE);
    std::memcpy(nv12.data(), shared->nv12, FRAME_NV12_SIZE);

    ::munmap(ptr, sizeof(SharedFrame));
    ::close(fd);

    cv::Mat yuv(FRAME_HEIGHT + FRAME_HEIGHT / 2, FRAME_WIDTH, CV_8UC1, nv12.data());
    cv::Mat bgr;

    try {
        cv::cvtColor(yuv, bgr, cv::COLOR_YUV2BGR_NV12);
    } catch (const cv::Exception& e) {
        error = "failed to convert NV12 to BGR: " + std::string(e.what());
        return false;
    }

    draw_roi(bgr, roi);
    draw_detection(bgr, event_id, detection);

    snapshot_path = join_path(snapshot_config.save_dir, event_id + ".jpg");

    try {
        if (!cv::imwrite(snapshot_path, bgr)) {
            error = "cv::imwrite returned false: " + snapshot_path;
            return false;
        }
    } catch (const cv::Exception& e) {
        error = "failed to write snapshot: " + std::string(e.what());
        return false;
    }

    return true;
}

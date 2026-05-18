#include "config.h"

#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

static bool read_text_file(const std::string& path, std::string& out, std::string& error) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        error = "failed to open config file: " + path;
        return false;
    }

    std::ostringstream oss;
    oss << ifs.rdbuf();
    out = oss.str();
    return true;
}

static std::string get_json_section(const std::string& text, const std::string& name) {
    const std::string key = "\"" + name + "\"";
    size_t key_pos = text.find(key);
    if (key_pos == std::string::npos) {
        return "";
    }

    size_t open_pos = text.find('{', key_pos);
    if (open_pos == std::string::npos) {
        return "";
    }

    int depth = 0;
    for (size_t i = open_pos; i < text.size(); ++i) {
        if (text[i] == '{') {
            ++depth;
        } else if (text[i] == '}') {
            --depth;
            if (depth == 0) {
                return text.substr(open_pos, i - open_pos + 1);
            }
        }
    }

    return "";
}

static bool get_string_value(const std::string& section, const std::string& key, std::string& value) {
    std::regex re("\"" + key + "\"\\s*:\\s*\"([^\"]*)\"");
    std::smatch match;
    if (std::regex_search(section, match, re)) {
        value = match[1].str();
        return true;
    }
    return false;
}

static bool get_int_value(const std::string& section, const std::string& key, int& value) {
    std::regex re("\"" + key + "\"\\s*:\\s*(-?[0-9]+)");
    std::smatch match;
    if (std::regex_search(section, match, re)) {
        value = std::stoi(match[1].str());
        return true;
    }
    return false;
}

static bool get_double_value(const std::string& section, const std::string& key, double& value) {
    std::regex re("\"" + key + "\"\\s*:\\s*(-?[0-9]+(\\.[0-9]+)?)");
    std::smatch match;
    if (std::regex_search(section, match, re)) {
        value = std::stod(match[1].str());
        return true;
    }
    return false;
}

static bool get_bool_value(const std::string& section, const std::string& key, bool& value) {
    std::regex re("\"" + key + "\"\\s*:\\s*(true|false)");
    std::smatch match;
    if (std::regex_search(section, match, re)) {
        value = (match[1].str() == "true");
        return true;
    }
    return false;
}

static void parse_roi_points(const std::string& rule_section, std::vector<RoiPoint>& roi) {
    roi.clear();

    std::regex pair_re("\\[\\s*(-?[0-9]+)\\s*,\\s*(-?[0-9]+)\\s*\\]");
    auto begin = std::sregex_iterator(rule_section.begin(), rule_section.end(), pair_re);
    auto end = std::sregex_iterator();

    for (auto it = begin; it != end; ++it) {
        RoiPoint p;
        p.x = std::stoi((*it)[1].str());
        p.y = std::stoi((*it)[2].str());
        roi.push_back(p);
    }
}

bool load_app_config(const std::string& path, AppConfig& config, std::string& error) {
    std::string text;
    if (!read_text_file(path, text, error)) {
        return false;
    }

    const std::string camera = get_json_section(text, "camera");
    if (!camera.empty()) {
        get_string_value(camera, "device", config.camera.device);
        get_int_value(camera, "width", config.camera.width);
        get_int_value(camera, "height", config.camera.height);
        get_string_value(camera, "format", config.camera.format);
    }

    const std::string model = get_json_section(text, "model");
    if (!model.empty()) {
        get_string_value(model, "path", config.model.path);
        get_int_value(model, "input_width", config.model.input_width);
        get_int_value(model, "input_height", config.model.input_height);
        get_double_value(model, "conf_threshold", config.model.conf_threshold);
        get_double_value(model, "nms_threshold", config.model.nms_threshold);
    }

    const std::string rule = get_json_section(text, "rule");
    if (!rule.empty()) {
        get_string_value(rule, "target_class", config.rule.target_class);
        get_int_value(rule, "dwell_time_ms", config.rule.dwell_time_ms);
        get_int_value(rule, "cooldown_ms", config.rule.cooldown_ms);
        parse_roi_points(rule, config.rule.roi);
    }

    const std::string mqtt = get_json_section(text, "mqtt");
    if (!mqtt.empty()) {
        get_string_value(mqtt, "host", config.mqtt.host);
        get_int_value(mqtt, "port", config.mqtt.port);
        get_string_value(mqtt, "topic_alarm", config.mqtt.topic_alarm);
        get_string_value(mqtt, "topic_status", config.mqtt.topic_status);
        get_string_value(mqtt, "topic_metrics", config.mqtt.topic_metrics);
    }

    const std::string web = get_json_section(text, "web");
    if (!web.empty()) {
        get_int_value(web, "port", config.web.port);
    }

    const std::string snapshot = get_json_section(text, "snapshot");
    if (!snapshot.empty()) {
        get_bool_value(snapshot, "enable", config.snapshot.enable);
        get_string_value(snapshot, "save_dir", config.snapshot.save_dir);
    }

    if (config.rule.roi.empty()) {
        config.rule.roi.push_back({120, 80});
        config.rule.roi.push_back({540, 80});
        config.rule.roi.push_back({580, 420});
        config.rule.roi.push_back({90, 420});
    }

    return true;
}

void print_app_config(const AppConfig& config) {
    std::printf("[config] camera.device      : %s\n", config.camera.device.c_str());
    std::printf("[config] camera.width       : %d\n", config.camera.width);
    std::printf("[config] camera.height      : %d\n", config.camera.height);
    std::printf("[config] camera.format      : %s\n", config.camera.format.c_str());

    std::printf("[config] model.path         : %s\n", config.model.path.c_str());
    std::printf("[config] model.input        : %dx%d\n", config.model.input_width, config.model.input_height);
    std::printf("[config] model.conf         : %.3f\n", config.model.conf_threshold);
    std::printf("[config] model.nms          : %.3f\n", config.model.nms_threshold);

    std::printf("[config] rule.target_class  : %s\n", config.rule.target_class.c_str());
    std::printf("[config] rule.dwell_time_ms : %d\n", config.rule.dwell_time_ms);
    std::printf("[config] rule.cooldown_ms   : %d\n", config.rule.cooldown_ms);
    std::printf("[config] rule.roi           :");
    for (const auto& p : config.rule.roi) {
        std::printf(" [%d,%d]", p.x, p.y);
    }
    std::printf("\n");

    std::printf("[config] mqtt.host          : %s\n", config.mqtt.host.c_str());
    std::printf("[config] mqtt.port          : %d\n", config.mqtt.port);
    std::printf("[config] mqtt.topic_alarm   : %s\n", config.mqtt.topic_alarm.c_str());
    std::printf("[config] mqtt.topic_status  : %s\n", config.mqtt.topic_status.c_str());
    std::printf("[config] mqtt.topic_metrics : %s\n", config.mqtt.topic_metrics.c_str());

    std::printf("[config] web.port           : %d\n", config.web.port);

    std::printf("[config] snapshot.enable    : %s\n", config.snapshot.enable ? "true" : "false");
    std::printf("[config] snapshot.save_dir  : %s\n", config.snapshot.save_dir.c_str());
}

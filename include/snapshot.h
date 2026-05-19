#pragma once

#include "config.h"
#include "rule_engine.h"

#include <string>
#include <vector>

bool save_alarm_snapshot(
    const std::string& event_id,
    const RuleDetection& detection,
    const std::vector<RoiPoint>& roi,
    const SnapshotConfig& snapshot_config,
    std::string& snapshot_path,
    std::string& error
);

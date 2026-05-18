#include "rule_engine.h"

#include <cassert>
#include <cstdio>

static RuleConfig make_test_rule_config() {
    RuleConfig config;

    config.target_class = "person";
    config.roi = {
        {100, 100},
        {300, 100},
        {300, 300},
        {100, 300}
    };
    config.dwell_time_ms = 1000;
    config.cooldown_ms = 5000;

    return config;
}

int main() {
    RuleConfig config = make_test_rule_config();
    RuleEngine engine(config, 0.45);

    assert(engine.isInsideRoi(150, 150));
    assert(engine.isInsideRoi(100, 100));
    assert(engine.isInsideRoi(300, 200));
    assert(!engine.isInsideRoi(50, 150));
    assert(!engine.isInsideRoi(350, 150));

    RuleDetection car;
    car.class_name = "car";
    car.confidence = 0.90f;
    car.x1 = 120;
    car.y1 = 120;
    car.x2 = 220;
    car.y2 = 260;

    RuleResult r1 = engine.evaluate(car, 1000);
    assert(!r1.alarm);
    assert(r1.reason == "class_mismatch");

    RuleDetection low_conf;
    low_conf.class_name = "person";
    low_conf.confidence = 0.20f;
    low_conf.x1 = 120;
    low_conf.y1 = 120;
    low_conf.x2 = 220;
    low_conf.y2 = 260;

    RuleResult r2 = engine.evaluate(low_conf, 1000);
    assert(!r2.alarm);
    assert(r2.reason == "low_confidence");

    RuleDetection outside;
    outside.class_name = "person";
    outside.confidence = 0.90f;
    outside.x1 = 10;
    outside.y1 = 120;
    outside.x2 = 80;
    outside.y2 = 260;

    RuleResult r3 = engine.evaluate(outside, 1000);
    assert(!r3.alarm);
    assert(r3.reason == "outside_roi");

    RuleDetection inside;
    inside.class_name = "person";
    inside.confidence = 0.90f;
    inside.x1 = 120;
    inside.y1 = 120;
    inside.x2 = 220;
    inside.y2 = 260;

    RuleResult r4 = engine.evaluate(inside, 1000);
    assert(r4.inside_roi);
    assert(!r4.alarm);
    assert(r4.dwell_time_ms == 0);
    assert(r4.reason == "dwell_time_not_reached");

    RuleResult r5 = engine.evaluate(inside, 2100);
    assert(r5.inside_roi);
    assert(r5.alarm);
    assert(r5.dwell_time_ms == 1100);
    assert(r5.reason == "alarm");

    RuleResult r6 = engine.evaluate(inside, 3000);
    assert(r6.inside_roi);
    assert(!r6.alarm);
    assert(r6.reason == "cooldown");

    RuleResult r7 = engine.evaluate(inside, 8000);
    assert(r7.inside_roi);
    assert(r7.alarm);
    assert(r7.reason == "alarm");

    RuleResult r8 = engine.evaluate(outside, 9000);
    assert(!r8.inside_roi);
    assert(!r8.alarm);
    assert(r8.reason == "outside_roi");

    std::printf("[test_rule_engine] all tests passed\n");
    return 0;
}

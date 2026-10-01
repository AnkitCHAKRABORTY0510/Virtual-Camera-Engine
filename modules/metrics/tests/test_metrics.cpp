// Unit tests for the histogram, the SPSC ring and the timing analyzer.
#include <gtest/gtest.h>

#include <sstream>
#include <thread>

#include "vcam/metrics/histogram.hpp"
#include "vcam/metrics/spsc_ring.hpp"
#include "vcam/metrics/timing_metrics.hpp"

using vcam::FrameRecord;

TEST(LatencyHistogram, PercentilesAreAccurateToOneMicrosecond) {
    vcam::LatencyHistogram histogram;
    for (int us = 1; us <= 1000; ++us) {
        histogram.add(us * 1000 - 500);  // 0.5 µs .. 999.5 µs
    }
    EXPECT_EQ(histogram.count(), 1000u);
    EXPECT_NEAR(static_cast<double>(histogram.percentile_ns(50)), 500'000, 1'000);
    EXPECT_NEAR(static_cast<double>(histogram.percentile_ns(99)), 990'000, 1'000);
    EXPECT_EQ(histogram.max_ns(), 999'500);
    EXPECT_NEAR(histogram.mean_ns(), 500'000, 1);
}

TEST(LatencyHistogram, LargeValuesAndClamping) {
    vcam::LatencyHistogram histogram;
    histogram.add(-5);              // clamped to 0
    histogram.add(50'000'000);      // 50 ms
    histogram.add(20'000'000'000);  // 20 s (overflow bucket)
    EXPECT_EQ(histogram.min_ns(), 0);
    EXPECT_EQ(histogram.max_ns(), 20'000'000'000);
    EXPECT_LE(histogram.percentile_ns(100), histogram.max_ns());
    EXPECT_NEAR(static_cast<double>(histogram.percentile_ns(60)), 50'000'000, 100'000);
}

TEST(SpscRing, KeepsOrderAndReportsFull) {
    vcam::SpscRing<int> ring(3);  // rounded up to 4
    EXPECT_EQ(ring.capacity(), 4u);
    for (int i = 0; i < 4; ++i) {
        EXPECT_TRUE(ring.push(i));
    }
    EXPECT_FALSE(ring.push(99));
    int value = -1;
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(ring.pop(value));
        EXPECT_EQ(value, i);
    }
    EXPECT_FALSE(ring.pop(value));
}

TEST(SpscRing, TransfersBetweenThreadsWithoutLoss) {
    vcam::SpscRing<uint64_t> ring(1024);
    constexpr uint64_t kItems = 1'000'000;
    std::thread producer([&] {
        for (uint64_t i = 0; i < kItems; ++i) {
            while (!ring.push(i)) {
                std::this_thread::yield();
            }
        }
    });
    uint64_t expected = 0;
    uint64_t value = 0;
    while (expected < kItems) {
        if (ring.pop(value)) {
            ASSERT_EQ(value, expected);
            ++expected;
        }
    }
    producer.join();
}

namespace {
constexpr int64_t kPeriod = 33'333'333;

FrameRecord perfect(uint64_t slot, int64_t lateness = 0) {
    FrameRecord record;
    record.slot = slot;
    record.target_ns = static_cast<int64_t>(slot) * kPeriod;
    record.wake_ns = record.target_ns + lateness;
    record.publish_ns = record.target_ns + lateness;
    record.source_index = slot;
    return record;
}
}  // namespace

TEST(TimingAnalyzer, PerfectStreamHasZeroErrorAndExactFps) {
    vcam::TimingAnalyzer analyzer(vcam::make_rational(30, 1), 2'000'000, false);
    for (uint64_t n = 0; n < 301; ++n) {
        FrameRecord record = perfect(n);
        record.target_ns = vcam::rescale(static_cast<int64_t>(n), vcam::make_rational(1, 30),
                                         vcam::make_rational(1, 1'000'000'000), vcam::Rounding::Up);
        record.wake_ns = record.publish_ns = record.target_ns;
        analyzer.add(record);
    }
    vcam::TimingReport report = analyzer.report();
    EXPECT_EQ(report.frames_published, 301u);
    EXPECT_NEAR(report.measured_fps, 30.0, 1e-6);
    EXPECT_EQ(report.lateness.max_ns, 0);
    EXPECT_LE(report.jitter_abs.max_ns, 1);  // only ns rounding
    EXPECT_NEAR(report.drift_ppm, 0.0, 1e-9);
    EXPECT_EQ(report.late_frames, 0u);
}

TEST(TimingAnalyzer, DetectsLinearDrift) {
    // Lateness grows by 10 µs per second of stream = 10 ppm.
    vcam::TimingAnalyzer analyzer(vcam::make_rational(30, 1), 2'000'000, true);
    for (uint64_t n = 0; n < 3000; ++n) {
        const double seconds = static_cast<double>(n) * kPeriod / 1e9;
        analyzer.add(perfect(n, static_cast<int64_t>(seconds * 10'000)));
    }
    EXPECT_NEAR(analyzer.report().drift_ppm, 10.0, 0.01);
}

TEST(TimingAnalyzer, CountsEventsAndJitterAcrossSkippedSlots) {
    vcam::TimingAnalyzer analyzer(vcam::make_rational(30, 1), 1'000'000, true);
    analyzer.add(perfect(0));
    analyzer.add(perfect(1, 3'000'000));  // 3 ms late -> late frame, jitter 3 ms
    FrameRecord after_skip = perfect(3);  // slot 2 was skipped
    after_skip.missed_before = 1;
    analyzer.add(after_skip);
    FrameRecord repeated = perfect(4);
    repeated.flags = vcam::kFrameRepeated;
    analyzer.add(repeated);
    FrameRecord refused = perfect(5);
    refused.flags = vcam::kFrameBackpressure;
    analyzer.add(refused);

    vcam::TimingReport report = analyzer.report();
    EXPECT_EQ(report.slots, 5u);
    EXPECT_EQ(report.frames_published, 4u);  // backpressure frame not delivered
    EXPECT_EQ(report.late_frames, 1u);
    EXPECT_EQ(report.missed_slots, 1u);
    EXPECT_EQ(report.repeated_frames, 1u);
    EXPECT_EQ(report.backpressure_frames, 1u);
    // Jitter: slot 1 (+3 ms), slot 3 measured over a 2-slot gap (-3 ms), slot 4 (0).
    EXPECT_NEAR(static_cast<double>(report.jitter_abs.max_ns), 3'000'000, 1'000);
}

TEST(TimingAnalyzer, TextJsonAndCsvOutput) {
    vcam::TimingAnalyzer analyzer(vcam::make_rational(25, 1), 2'000'000, true);
    analyzer.add(perfect(0));
    analyzer.add(perfect(1));
    const vcam::TimingReport report = analyzer.report();
    const std::string text = vcam::format_report_text(report);
    EXPECT_NE(text.find("Requested FPS"), std::string::npos);
    EXPECT_NE(text.find("Drift"), std::string::npos);
    const std::string json = vcam::format_report_json(report);
    EXPECT_EQ(json.front(), '{');
    EXPECT_EQ(json.back(), '}');
    EXPECT_NE(json.find("\"lateness\":{"), std::string::npos);

    std::ostringstream csv;
    vcam::write_frame_csv_header(csv);
    vcam::write_frame_csv_row(csv, perfect(2, 5));
    EXPECT_NE(csv.str().find("\n2,66666666,66666671,66666671,5,2,0,0,0\n"), std::string::npos);
}

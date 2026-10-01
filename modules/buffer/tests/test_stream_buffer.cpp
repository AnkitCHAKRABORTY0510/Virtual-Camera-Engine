// Unit tests for StreamBuffer (the low-memory decode-ahead ring).
#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <thread>

#include "vcam/buffer/stream_buffer.hpp"

namespace {

// Writes one frame whose bytes all equal `value`, with pts = value * 10 ticks.
void push(vcam::StreamBuffer& buffer, uint8_t value) {
    ASSERT_TRUE(buffer.wait_for_space(1000));
    auto slot = buffer.begin_write();
    ASSERT_TRUE(slot.ok()) << slot.status().to_string();
    std::memset(slot->bytes.data(), value, slot->bytes.size());
    ASSERT_TRUE(buffer.commit_write(vcam::FrameTiming{0, static_cast<int64_t>(value) * 10, 10}).ok());
}

vcam::FrameFormat small_format() {
    return vcam::make_frame_format(16, 16, vcam::PixelFormat::GRAY8).value();
}

}  // namespace

TEST(StreamBuffer, PicksTheFrameOnScreenAndFreesOlderOnes) {
    vcam::StreamBuffer buffer(4);
    ASSERT_TRUE(buffer.configure(small_format(), {}).ok());
    for (uint8_t v = 0; v < 4; ++v) push(buffer, v);  // pts 0, 10, 20, 30
    EXPECT_TRUE(buffer.is_full());
    EXPECT_FALSE(buffer.wait_for_space(10));          // writer must wait

    vcam::StreamPick first = buffer.pick(5);           // time 5 -> frame pts 0
    ASSERT_TRUE(first.lease);
    EXPECT_EQ(first.lease.view().bytes[0], 0);
    EXPECT_TRUE(first.new_frame);

    vcam::StreamPick same = buffer.pick(9);            // still frame 0
    EXPECT_FALSE(same.new_frame);

    vcam::StreamPick jump = buffer.pick(25);           // frame pts 20, frame 1 skipped
    EXPECT_EQ(jump.lease.view().bytes[0], 2);
    EXPECT_EQ(jump.skipped, 1u);
    EXPECT_TRUE(buffer.wait_for_space(10));            // frames 0 and 1 were freed
    EXPECT_EQ(buffer.buffered_frames(), 2u);           // frames 2 (shown) and 3
}

TEST(StreamBuffer, UnderflowWhenTheDecoderIsLate) {
    vcam::StreamBuffer buffer(4);
    ASSERT_TRUE(buffer.configure(small_format(), {}).ok());
    push(buffer, 0);
    push(buffer, 1);                                   // frames at 0 and 10, each 10 long
    EXPECT_FALSE(buffer.pick(15).underflow);           // frame 1 is on screen, fine
    vcam::StreamPick late = buffer.pick(25);           // frame 2 due at 20, not decoded
    EXPECT_TRUE(late.underflow);
    EXPECT_EQ(late.lease.view().bytes[0], 1);          // previous frame repeated
    push(buffer, 2);
    EXPECT_FALSE(buffer.pick(26).underflow);
}

TEST(StreamBuffer, PastEndOnlyWhenComplete) {
    vcam::StreamBuffer buffer(4);
    ASSERT_TRUE(buffer.configure(small_format(), {}).ok());
    push(buffer, 0);
    buffer.mark_complete();
    vcam::StreamPick end = buffer.pick(50);
    EXPECT_TRUE(end.past_end);
    EXPECT_FALSE(end.underflow);
    EXPECT_EQ(end.lease.view().bytes[0], 0);
}

TEST(StreamBuffer, RestartKeepsTheShownFrameUntilNewFramesArrive) {
    vcam::StreamBuffer buffer(4);
    ASSERT_TRUE(buffer.configure(small_format(), {}).ok());
    for (uint8_t v = 0; v < 4; ++v) push(buffer, v);
    buffer.pick(15);                                    // showing frame 1
    buffer.restart();                                   // seek: drop 2 and 3
    EXPECT_EQ(buffer.buffered_frames(), 0u);

    vcam::StreamPick waiting = buffer.pick(1000);
    EXPECT_TRUE(waiting.waiting);
    EXPECT_EQ(waiting.lease.view().bytes[0], 1);        // old frame still shown, intact

    // New frames after the seek have earlier timestamps; they are used anyway.
    push(buffer, 7);                                    // pts 70
    vcam::StreamPick after = buffer.pick(75);
    EXPECT_TRUE(after.new_frame);
    EXPECT_EQ(after.lease.view().bytes[0], 7);
    EXPECT_EQ(after.skipped, 0u);
}

TEST(StreamBuffer, MemoryIsCapacityTimesFrameSize) {
    vcam::StreamBuffer buffer(30);
    auto format = vcam::make_frame_format(800, 410, vcam::PixelFormat::YUYV).value();
    ASSERT_TRUE(buffer.configure(format, {}).ok());
    EXPECT_EQ(buffer.stats().memory_bytes, 30u * 656000u);  // ~19 MiB for any video length
    EXPECT_EQ(buffer.stats().kind, "stream");
}

TEST(StreamBuffer, WriterAndReaderInParallel) {
    // The writer fills as fast as it can; the reader advances time. Every
    // picked frame must be complete (no torn frames) and in order.
    vcam::StreamBuffer buffer(8);
    auto format = vcam::make_frame_format(64, 64, vcam::PixelFormat::GRAY8).value();
    ASSERT_TRUE(buffer.configure(format, {}).ok());
    constexpr int kFrames = 3000;
    std::thread writer([&] {
        for (int i = 0; i < kFrames; ++i) {
            while (!buffer.wait_for_space(100)) {
            }
            auto slot = buffer.begin_write();
            std::memset(slot->bytes.data(), i & 0xFF, slot->bytes.size());
            buffer.commit_write(vcam::FrameTiming{0, i, 1});
        }
        buffer.mark_complete();
    });
    int previous = -1;
    int torn = 0;
    int backwards = 0;
    for (int64_t t = 0; t < kFrames + 10; ++t) {
        vcam::StreamPick pick;
        do {
            pick = buffer.pick(t);
        } while (pick.underflow || (pick.waiting && !pick.lease));  // wait for the writer
        const auto& bytes = pick.lease.view().bytes;
        for (uint8_t b : bytes) {
            if (b != bytes[0]) { ++torn; break; }
        }
        const int value = static_cast<int>(pick.lease.view().timing.pts_ticks);
        if (value < previous) ++backwards;
        previous = value;
        if (pick.past_end) break;
    }
    writer.join();
    EXPECT_EQ(torn, 0);
    EXPECT_EQ(backwards, 0);
    EXPECT_EQ(previous, kFrames - 1);
}

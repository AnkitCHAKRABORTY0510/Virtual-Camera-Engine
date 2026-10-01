// Unit tests for LiveFrameBuffer (newest-frame buffer for live sources).
#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <thread>

#include "vcam/buffer/live_frame_buffer.hpp"

namespace {

void write_frame(vcam::LiveFrameBuffer& buffer, uint8_t value) {
    auto slot = buffer.begin_write();
    ASSERT_TRUE(slot.ok()) << slot.status().to_string();
    std::memset(slot->bytes.data(), value, slot->bytes.size());
    ASSERT_TRUE(buffer.commit_write({}).ok());
}

bool uniform(const vcam::FrameView& view) {
    for (uint8_t byte : view.bytes) {
        if (byte != view.bytes[0]) {
            return false;
        }
    }
    return true;
}

}  // namespace

TEST(LiveFrameBuffer, NewestFrameWinsAndOldFramesAreRecycled) {
    vcam::LiveFrameBuffer buffer(3);
    ASSERT_TRUE(buffer.configure(vcam::make_frame_format(32, 32, vcam::PixelFormat::GRAY8).value(), {}).ok());
    EXPECT_FALSE(buffer.newest_index().has_value());

    for (uint8_t i = 0; i < 10; ++i) {
        write_frame(buffer, i);
    }
    EXPECT_EQ(buffer.frame_count(), 10u);
    EXPECT_EQ(buffer.newest_index().value(), 9u);
    vcam::FrameLease newest = buffer.acquire(9);
    ASSERT_TRUE(newest);
    EXPECT_EQ(newest.view().bytes[0], 9);
    EXPECT_FALSE(buffer.acquire(0));  // long overwritten
    EXPECT_EQ(buffer.timeline(), nullptr);
}

TEST(LiveFrameBuffer, PinnedFrameIsNeverOverwritten) {
    vcam::LiveFrameBuffer buffer(3);
    ASSERT_TRUE(buffer.configure(vcam::make_frame_format(32, 32, vcam::PixelFormat::GRAY8).value(), {}).ok());
    write_frame(buffer, 1);
    vcam::FrameLease pinned = buffer.acquire(0);
    ASSERT_TRUE(pinned);
    for (uint8_t i = 2; i < 50; ++i) {
        write_frame(buffer, i);  // must always find a free slot
    }
    EXPECT_EQ(pinned.view().bytes[0], 1);  // untouched while pinned
    pinned.reset();
    write_frame(buffer, 99);
    EXPECT_EQ(buffer.acquire(buffer.newest_index().value()).view().bytes[0], 99);
}

TEST(LiveFrameBuffer, NoTornFramesUnderConcurrency) {
    vcam::LiveFrameBuffer buffer(3);
    ASSERT_TRUE(buffer.configure(vcam::make_frame_format(128, 128, vcam::PixelFormat::GRAY8).value(), {}).ok());
    std::atomic<bool> done{false};
    std::atomic<int> torn{0};
    std::atomic<int> reads{0};
    std::thread reader([&] {
        while (!done.load()) {
            if (auto newest = buffer.newest_index()) {
                vcam::FrameLease lease = buffer.acquire(*newest);
                if (lease) {
                    ++reads;
                    if (!uniform(lease.view())) {
                        ++torn;
                    }
                }
            }
        }
    });
    // Keep writing until the reader has really overlapped with the writer
    // (on a busy machine the reader thread may start late).
    for (int i = 0; i < 5000 || (reads.load() < 200 && i < 5'000'000); ++i) {
        write_frame(buffer, static_cast<uint8_t>(i));
    }
    done.store(true);
    reader.join();
    EXPECT_EQ(torn.load(), 0);
    EXPECT_GT(reads.load(), 0);
}

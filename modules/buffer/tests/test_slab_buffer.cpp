// Unit tests for RamBuffer and DiskBackedBuffer (full-preload buffers).
#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <memory>
#include <thread>

#include "vcam/buffer/slab_buffer.hpp"

using vcam::FrameTiming;
using vcam::PixelFormat;

namespace {

// Writes `count` frames whose every byte equals (index & 0xFF), then completes.
void fill(vcam::FrameBuffer& buffer, uint64_t count) {
    for (uint64_t i = 0; i < count; ++i) {
        auto slot = buffer.begin_write();
        ASSERT_TRUE(slot.ok()) << slot.status().to_string();
        std::memset(slot->bytes.data(), static_cast<int>(i & 0xFF), slot->bytes.size());
        ASSERT_TRUE(buffer.commit_write(FrameTiming{i, static_cast<int64_t>(i) * 100, 100}).ok());
    }
    buffer.mark_complete();
}

bool frame_has_value(const vcam::FrameView& view, uint8_t value) {
    for (uint8_t byte : view.bytes) {
        if (byte != value) {
            return false;
        }
    }
    return true;
}

// The same behaviour tests run for both buffer kinds.
class SlabBufferTest : public ::testing::TestWithParam<std::string> {
protected:
    std::unique_ptr<vcam::SlabBuffer> make(uint64_t max_bytes) {
        if (GetParam() == "ram") {
            return std::make_unique<vcam::RamBuffer>(max_bytes);
        }
        spill_dir_ = std::filesystem::temp_directory_path() / ("vcam_test_" + std::to_string(::getpid()));
        std::filesystem::create_directories(spill_dir_);
        auto disk = std::make_unique<vcam::DiskBackedBuffer>(spill_dir_.string(), max_bytes);
        EXPECT_TRUE(disk->open().ok());
        return disk;
    }
    void TearDown() override {
        if (!spill_dir_.empty()) {
            // The spill file was unlinked at creation: the directory must be empty.
            EXPECT_TRUE(std::filesystem::is_empty(spill_dir_));
            std::filesystem::remove_all(spill_dir_);
        }
    }
    std::filesystem::path spill_dir_;
};

}  // namespace

TEST_P(SlabBufferTest, StoresAndReturnsEveryFrame) {
    auto format = vcam::make_frame_format(320, 240, PixelFormat::YUYV).value();
    auto buffer = make(1ull << 30);
    ASSERT_TRUE(buffer->configure(format, 50).ok());
    fill(*buffer, 50);

    EXPECT_TRUE(buffer->is_complete());
    EXPECT_EQ(buffer->frame_count(), 50u);
    EXPECT_EQ(buffer->newest_index().value(), 49u);
    for (uint64_t i = 0; i < 50; ++i) {
        vcam::FrameLease lease = buffer->acquire(i);
        ASSERT_TRUE(lease);
        EXPECT_TRUE(frame_has_value(lease.view(), static_cast<uint8_t>(i))) << "frame " << i;
        EXPECT_EQ(lease.view().timing.pts_ticks, static_cast<int64_t>(i) * 100);
    }
    EXPECT_FALSE(buffer->acquire(50));  // beyond the end
    ASSERT_NE(buffer->timeline(), nullptr);
    EXPECT_EQ(buffer->timeline()->size(), 50u);
}

TEST_P(SlabBufferTest, SpansSeveralSlabs) {
    // 4 MiB frames: ~16 per 64 MiB slab, so 40 frames need 3 slabs.
    auto format = vcam::make_frame_format(1920, 1080, PixelFormat::YUYV).value();
    auto buffer = make(1ull << 30);
    ASSERT_TRUE(buffer->configure(format, std::nullopt).ok());
    fill(*buffer, 40);
    EXPECT_LT(buffer->frames_per_slab(), 40u);
    for (uint64_t i : {0ull, 15ull, 16ull, 17ull, 39ull}) {
        EXPECT_TRUE(frame_has_value(buffer->acquire(i).view(), static_cast<uint8_t>(i))) << "frame " << i;
    }
    const vcam::BufferStats stats = buffer->stats();
    EXPECT_EQ(stats.frames_loaded, 40u);
    EXPECT_GE(stats.memory_bytes + stats.disk_bytes, 40u * format.size_bytes);
}

TEST_P(SlabBufferTest, RefusesToExceedBudget) {
    auto format = vcam::make_frame_format(320, 240, PixelFormat::YUYV).value();  // 153 600 B
    auto buffer = make(10 * 153'600);
    ASSERT_TRUE(buffer->configure(format, std::nullopt).ok());
    for (int i = 0; i < 10; ++i) {
        ASSERT_TRUE(buffer->begin_write().ok());
        ASSERT_TRUE(buffer->commit_write({}).ok());
    }
    auto eleventh = buffer->begin_write();
    EXPECT_EQ(eleventh.status().code(), vcam::StatusCode::ResourceExhausted);
}

TEST_P(SlabBufferTest, ClearResets) {
    auto format = vcam::make_frame_format(64, 64, PixelFormat::GRAY8).value();
    auto buffer = make(1ull << 24);
    ASSERT_TRUE(buffer->configure(format, 5).ok());
    fill(*buffer, 5);
    buffer->clear();
    EXPECT_EQ(buffer->frame_count(), 0u);
    EXPECT_FALSE(buffer->is_complete());
    EXPECT_FALSE(buffer->acquire(0));
    fill(*buffer, 3);  // usable again
    EXPECT_EQ(buffer->frame_count(), 3u);
}

TEST_P(SlabBufferTest, ReaderSeesOnlyCompleteFramesWhileWriting) {
    // One writer and one reader at the same time (run under TSan to check races).
    auto format = vcam::make_frame_format(64, 64, PixelFormat::GRAY8).value();
    auto buffer = make(1ull << 26);
    ASSERT_TRUE(buffer->configure(format, std::nullopt).ok());

    std::atomic<bool> done{false};
    std::atomic<int> bad_frames{0};
    std::thread reader([&] {
        while (!done.load()) {
            if (auto newest = buffer->newest_index()) {
                vcam::FrameLease lease = buffer->acquire(*newest);
                if (lease && !frame_has_value(lease.view(), static_cast<uint8_t>(*newest))) {
                    ++bad_frames;
                }
            }
        }
    });
    fill(*buffer, 2000);
    done.store(true);
    reader.join();
    EXPECT_EQ(bad_frames.load(), 0);
}

INSTANTIATE_TEST_SUITE_P(Kinds, SlabBufferTest, ::testing::Values("ram", "disk"),
                         [](const auto& test_info) { return test_info.param; });

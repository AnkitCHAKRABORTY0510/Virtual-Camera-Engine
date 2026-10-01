// Tests for PatternSource + barcode, ImageSequenceSource, PushSource and the factory.
#include <gtest/gtest.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <thread>

#include "vcam/core/log.hpp"
#include "vcam/source/frame_barcode.hpp"
#include "vcam/source/image_sequence_source.hpp"
#include "vcam/source/pattern_source.hpp"
#include "vcam/source/push_source.hpp"
#include "vcam/source/source_factory.hpp"

using vcam::PixelFormat;

namespace {

std::string media(const std::string& name) {
    const char* directory = std::getenv("VCAM_TEST_MEDIA_DIR");
    return std::string(directory ? directory : "test_media") + "/" + name;
}

}  // namespace

// ---- Pattern + barcode ------------------------------------------------------------------

TEST(PatternSource, BarcodeSurvivesEveryPixelFormat) {
    for (PixelFormat format : {PixelFormat::YUYV, PixelFormat::UYVY, PixelFormat::I420, PixelFormat::NV12,
                               PixelFormat::RGB24, PixelFormat::BGR24, PixelFormat::GRAY8}) {
        SCOPED_TRACE(vcam::pixel_format_name(format));
        vcam::PatternOptions options;
        options.width = 320;
        options.height = 240;
        options.frame_count = 40;
        vcam::PatternSource source(options);
        ASSERT_TRUE(source.open().ok());
        auto output = vcam::make_output_format(source.info(), format).value();
        ASSERT_TRUE(source.set_output_format(output).ok());
        vcam::OwnedFrame frame(output);
        for (uint64_t i = 0; i < 40; ++i) {
            vcam::FrameTiming timing;
            ASSERT_TRUE(source.read_next(frame.mutable_view(), timing).is_frame());
            EXPECT_EQ(timing.pts_ticks, static_cast<int64_t>(i));
            auto code = vcam::read_barcode(frame.view());
            ASSERT_TRUE(code.has_value()) << "frame " << i;
            EXPECT_EQ(*code, i);
        }
        vcam::FrameTiming timing;
        EXPECT_TRUE(source.read_next(frame.mutable_view(), timing).is_end_of_stream());
    }
}

TEST(FrameBarcode, RejectsFramesWithoutBarcode) {
    auto format = vcam::make_frame_format(320, 240, PixelFormat::GRAY8).value();
    vcam::OwnedFrame frame(format);
    std::memset(frame.mutable_view().bytes.data(), 0x80, format.size_bytes);  // flat grey
    EXPECT_FALSE(vcam::read_barcode(frame.view()).has_value());
}

// ---- Image sequences ------------------------------------------------------------------------

TEST(NaturalSort, NumbersCompareByValue) {
    EXPECT_TRUE(vcam::natural_less("frame2.png", "frame10.png"));
    EXPECT_FALSE(vcam::natural_less("frame10.png", "frame2.png"));
    EXPECT_TRUE(vcam::natural_less("001.png", "002.png"));
    EXPECT_TRUE(vcam::natural_less("a.png", "b.png"));
    EXPECT_TRUE(vcam::natural_less("img9", "img09x"));
}

TEST(ImageSequenceSource, DecodesDirectoryInOrder) {
    if (!std::filesystem::exists(media("images"))) {
        GTEST_SKIP() << "test media missing";
    }
    vcam::log::set_level(vcam::LogLevel::Error);
    vcam::ImageSequenceSource source(media("images"), vcam::make_rational(25, 1));
    ASSERT_TRUE(source.open().ok());
    EXPECT_EQ(source.info().width, 320);
    EXPECT_EQ(source.info().frame_count_estimate.value(), 12u);
    EXPECT_EQ(source.info().nominal_fps.value(), vcam::make_rational(25, 1));
    auto output = vcam::make_output_format(source.info(), PixelFormat::YUYV).value();
    ASSERT_TRUE(source.set_output_format(output).ok());
    vcam::OwnedFrame frame(output);
    int count = 0;
    vcam::FrameTiming timing;
    while (source.read_next(frame.mutable_view(), timing).is_frame()) {
        EXPECT_EQ(timing.pts_ticks, count);
        ++count;
    }
    EXPECT_EQ(count, 12);
}

TEST(ImageSequenceSource, DifferentImageSizeIsAnError) {
    if (!std::filesystem::exists(media("images_mixed"))) {
        GTEST_SKIP() << "test media missing";
    }
    vcam::ImageSequenceSource source(media("images_mixed"), vcam::make_rational(30, 1));
    ASSERT_TRUE(source.open().ok());
    auto output = vcam::make_output_format(source.info(), PixelFormat::RGB24).value();
    ASSERT_TRUE(source.set_output_format(output).ok());
    vcam::OwnedFrame frame(output);
    vcam::FrameTiming timing;
    EXPECT_TRUE(source.read_next(frame.mutable_view(), timing).is_frame());
    EXPECT_TRUE(source.read_next(frame.mutable_view(), timing).is_frame());
    vcam::ReadResult third = source.read_next(frame.mutable_view(), timing);
    ASSERT_TRUE(third.is_error());
    EXPECT_NE(third.status().message().find("same size"), std::string::npos);
}

TEST(ImageSequenceSource, EmptyOrMissingDirectory) {
    vcam::ImageSequenceSource missing("/nonexistent/dir", vcam::make_rational(30, 1));
    EXPECT_EQ(missing.open().code(), vcam::StatusCode::NotFound);
    const auto empty = std::filesystem::temp_directory_path() / "vcam_empty_images";
    std::filesystem::create_directories(empty);
    vcam::ImageSequenceSource none(empty.string(), vcam::make_rational(30, 1));
    EXPECT_EQ(none.open().code(), vcam::StatusCode::NotFound);
    std::filesystem::remove_all(empty);
}

// ---- Push source (script input) -----------------------------------------------------------

namespace {

// Minimal C++ producer speaking the documented protocol.
class TestProducer {
public:
    explicit TestProducer(const std::string& path) {
        fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
        for (int attempt = 0; attempt < 100; ++attempt) {  // the engine may not listen yet
            if (connect(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
                connected_ = true;
                break;
            }
            usleep(20'000);
        }
    }
    ~TestProducer() { if (fd_ >= 0) close(fd_); }

    bool hello(uint16_t pixel_code, uint32_t width, uint32_t height, std::string& reply_code) {
        uint8_t message[16] = {'V', 'C', 'A', 'M', 1, 0, static_cast<uint8_t>(pixel_code), 0};
        std::memcpy(message + 8, &width, 4);   // x86-64 is little-endian
        std::memcpy(message + 12, &height, 4);
        if (!send_all(message, 16)) return false;
        uint8_t reply[8];
        if (recv(fd_, reply, 8, MSG_WAITALL) != 8) return false;
        reply_code.assign(reinterpret_cast<char*>(reply), 2);
        return true;
    }
    bool frame(const std::vector<uint8_t>& pixels) {
        uint8_t header[8] = {'F', 'R', 'A', 'M'};
        const auto size = static_cast<uint32_t>(pixels.size());
        std::memcpy(header + 4, &size, 4);
        return send_all(header, 8) && send_all(pixels.data(), pixels.size());
    }
    bool connected() const { return connected_; }

private:
    bool send_all(const uint8_t* data, size_t size) {
        size_t sent = 0;
        while (sent < size) {
            const ssize_t n = send(fd_, data + sent, size - sent, MSG_NOSIGNAL);
            if (n <= 0) return false;
            sent += static_cast<size_t>(n);
        }
        return true;
    }
    int fd_ = -1;
    bool connected_ = false;
};

}  // namespace

TEST(PushSource, ReceivesFramesFromAProducer) {
    vcam::log::set_level(vcam::LogLevel::Error);
    const std::string path = (std::filesystem::temp_directory_path() / "vcam_test_push.sock").string();
    vcam::PushSource source(path);

    // Producer: 64x32 GRAY8 frames whose bytes all equal the frame number.
    std::thread producer([&] {
        TestProducer client(path);
        ASSERT_TRUE(client.connected());
        std::string reply;
        ASSERT_TRUE(client.hello(6 /* gray8 */, 64, 32, reply));
        ASSERT_EQ(reply, "OK");
        for (uint8_t value = 1; value <= 5; ++value) {
            ASSERT_TRUE(client.frame(std::vector<uint8_t>(64 * 32, value)));
        }
    });

    ASSERT_TRUE(source.open().ok());  // blocks until HELLO
    EXPECT_EQ(source.info().width, 64);
    EXPECT_TRUE(source.info().is_live);
    auto output = vcam::make_output_format(source.info(), PixelFormat::GRAY8).value();
    ASSERT_TRUE(source.set_output_format(output).ok());

    vcam::OwnedFrame frame(output);
    std::vector<uint8_t> received;
    for (int attempt = 0; attempt < 100 && received.size() < 5; ++attempt) {
        vcam::FrameTiming timing;
        vcam::ReadResult result = source.read_next(frame.mutable_view(), timing);
        ASSERT_FALSE(result.is_error()) << result.status().to_string();
        if (result.is_frame()) {
            received.push_back(frame.view().bytes[0]);
        }
    }
    producer.join();
    EXPECT_EQ(received, (std::vector<uint8_t>{1, 2, 3, 4, 5}));

    // After the producer left, reads just report Again (camera holds last frame).
    vcam::FrameTiming timing;
    EXPECT_TRUE(source.read_next(frame.mutable_view(), timing).is_again());
    source.close();
    EXPECT_FALSE(std::filesystem::exists(path));  // socket file cleaned up
}

TEST(PushSource, SecondProducerMustKeepTheSize) {
    vcam::log::set_level(vcam::LogLevel::Error);
    const std::string path = (std::filesystem::temp_directory_path() / "vcam_test_push2.sock").string();
    vcam::PushSource source(path);
    std::thread first([&] {
        TestProducer client(path);
        std::string reply;
        ASSERT_TRUE(client.hello(4 /* rgb24 */, 64, 32, reply));
        EXPECT_EQ(reply, "OK");
    });
    ASSERT_TRUE(source.open().ok());
    first.join();
    ASSERT_TRUE(source.set_output_format(vcam::make_output_format(source.info(), PixelFormat::YUYV).value()).ok());

    std::string second_reply;              // written by the thread, read after join()
    std::atomic<bool> replied{false};      // safe to poll from this thread
    std::thread second([&] {
        TestProducer client(path);
        client.hello(4, 128, 64, second_reply);
        replied.store(true);
    });
    vcam::OwnedFrame frame(vcam::make_output_format(source.info(), PixelFormat::YUYV).value());
    for (int i = 0; i < 30 && !replied.load(); ++i) {
        vcam::FrameTiming timing;
        source.read_next(frame.mutable_view(), timing);
    }
    second.join();
    EXPECT_EQ(second_reply, "ER");
}

TEST(PushSource, InterruptStopsWaiting) {
    const std::string path = (std::filesystem::temp_directory_path() / "vcam_test_push3.sock").string();
    vcam::PushSource source(path);
    std::thread stopper([&] {
        usleep(200'000);
        source.interrupt();
    });
    EXPECT_EQ(source.open().code(), vcam::StatusCode::Cancelled);
    stopper.join();
}

// ---- Factory ----------------------------------------------------------------------------------

TEST(SourceFactory, ResolvesTypes) {
    vcam::SourceSpec spec;
    spec.path = "pattern";
    EXPECT_EQ(vcam::resolve_source_type(spec), vcam::SourceType::Pattern);
    spec.path = std::filesystem::temp_directory_path().string();
    EXPECT_EQ(vcam::resolve_source_type(spec), vcam::SourceType::Images);
    spec.path = "clip.mp4";
    EXPECT_EQ(vcam::resolve_source_type(spec), vcam::SourceType::Video);
    spec.type = vcam::SourceType::Push;
    EXPECT_EQ(vcam::resolve_source_type(spec), vcam::SourceType::Push);
    EXPECT_FALSE(vcam::parse_source_type("camera").ok());
    EXPECT_TRUE(vcam::create_source(spec).ok());
}

TEST(PatternSource, SeekJumpsToFrame) {
    vcam::PatternOptions options;
    options.width = 128;
    options.height = 64;
    options.frame_count = 50;
    vcam::PatternSource source(options);
    ASSERT_TRUE(source.open().ok());
    auto output = vcam::make_output_format(source.info(), PixelFormat::GRAY8).value();
    ASSERT_TRUE(source.set_output_format(output).ok());
    vcam::OwnedFrame frame(output);
    vcam::FrameTiming timing;
    ASSERT_TRUE(source.seek(42).ok());
    ASSERT_TRUE(source.read_next(frame.mutable_view(), timing).is_frame());
    EXPECT_EQ(vcam::read_barcode(frame.view()).value(), 42u);
    ASSERT_TRUE(source.seek(0).ok());
    ASSERT_TRUE(source.read_next(frame.mutable_view(), timing).is_frame());
    EXPECT_EQ(timing.pts_ticks, 0);
}

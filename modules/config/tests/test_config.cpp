// Unit tests for settings, the YAML-subset config file and the command line.
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <vector>

#include "vcam/config/command_line.hpp"
#include "vcam/config/config_file.hpp"

using vcam::Config;

namespace {

// Builds a writable argv from string literals.
struct Args {
    explicit Args(std::vector<std::string> words) : storage(std::move(words)) {
        for (std::string& word : storage) pointers.push_back(word.data());
        pointers.push_back(nullptr);
    }
    int argc() const { return static_cast<int>(storage.size()); }
    char** argv() { return pointers.data(); }
    std::vector<std::string> storage;
    std::vector<char*> pointers;
};

}  // namespace

TEST(Settings, ApplyAndReject) {
    Config config;
    EXPECT_TRUE(vcam::apply_setting(config, "output.fps", "29.97").ok());
    EXPECT_EQ(config.output.fps.value(), vcam::make_rational(30000, 1001));
    EXPECT_TRUE(vcam::apply_setting(config, "playback.on_eof", "hold").ok());
    EXPECT_EQ(config.playback.on_eof, vcam::EofPolicy::Hold);
    EXPECT_TRUE(vcam::apply_setting(config, "buffer.memory_limit", "2G").ok());
    EXPECT_EQ(config.buffer.memory_limit, 2ull << 30);
    EXPECT_TRUE(vcam::apply_setting(config, "input.pattern_size", "640x360").ok());
    EXPECT_EQ(config.input.pattern_height, 360);

    vcam::Status bad = vcam::apply_setting(config, "output.fps", "-5");
    EXPECT_FALSE(bad.ok());
    EXPECT_NE(bad.message().find("output.fps"), std::string::npos);
    EXPECT_FALSE(vcam::apply_setting(config, "output.colour", "red").ok());
    EXPECT_FALSE(vcam::apply_setting(config, "playback.on_eof", "rewind").ok());
}

TEST(Settings, ValidateFastModeNeedsAnEnd) {
    Config config;
    config.input.path = "clip.mp4";
    config.playback.realtime = false;
    config.playback.on_eof = vcam::EofPolicy::Loop;
    EXPECT_FALSE(vcam::validate(config).ok());
    config.playback.max_frames = 100;
    EXPECT_TRUE(vcam::validate(config).ok());
}

TEST(Settings, ValidateRequiresInput) {
    Config config;
    EXPECT_FALSE(vcam::validate(config).ok());
    config.input.type = vcam::SourceType::Push;
    EXPECT_TRUE(vcam::validate(config).ok());  // push uses the default socket
}

TEST(ConfigFile, ParsesSectionsCommentsAndQuotes) {
    const std::string text =
        "# example\n"
        "input:\n"
        "  type: video\n"
        "  path: /videos/clip.mp4   # trailing comment\n"
        "\n"
        "output:\n"
        "  fps: 30\n"
        "  device_name: \"My # Camera\"\n";
    auto entries = vcam::parse_config_text(text);
    ASSERT_TRUE(entries.ok()) << entries.status().to_string();
    ASSERT_EQ(entries->size(), 4u);
    EXPECT_EQ(entries.value()[1].key, "input.path");
    EXPECT_EQ(entries.value()[1].value, "/videos/clip.mp4");
    EXPECT_EQ(entries.value()[3].value, "My # Camera");
    EXPECT_EQ(entries.value()[3].line, 8);
}

TEST(ConfigFile, ReportsLineNumbers) {
    auto list = vcam::parse_config_text("input:\n  - a\n");
    ASSERT_FALSE(list.ok());
    EXPECT_NE(list.status().message().find("line 2"), std::string::npos);
    EXPECT_FALSE(vcam::parse_config_text("fps: 30\n").ok());  // top-level value
}

TEST(CommandLine, ConfigFileThenOptionsOverride) {
    const auto path = std::filesystem::temp_directory_path() / "vcam_test_config.yaml";
    std::ofstream(path) << "output:\n  fps: 25\n  pixel_format: i420\nplayback:\n  on_eof: stop\n";

    Args args({"virtual-camera", "--fps", "60", "--config", path.string(), "-i", "clip.mp4", "--set",
               "timing.late_ms=5"});
    auto parsed = vcam::parse_command_line(args.argc(), args.argv());
    ASSERT_TRUE(parsed.ok()) << parsed.status().to_string();
    const Config& config = parsed->config;
    EXPECT_EQ(config.output.fps.value(), vcam::make_rational(60, 1));    // CLI wins over file
    EXPECT_EQ(config.output.pixel_format, vcam::PixelFormat::I420);       // from the file
    EXPECT_EQ(config.playback.on_eof, vcam::EofPolicy::Stop);             // from the file
    EXPECT_EQ(config.input.path, "clip.mp4");
    EXPECT_DOUBLE_EQ(config.timing.late_ms, 5.0);
    std::filesystem::remove(path);
}

TEST(CommandLine, FlagsBareInputAndErrors) {
    Args flags({"virtual-camera", "--fast", "--quiet", "--verbose", "video.mp4"});
    auto parsed = vcam::parse_command_line(flags.argc(), flags.argv());
    ASSERT_TRUE(parsed.ok()) << parsed.status().to_string();
    EXPECT_FALSE(parsed->config.playback.realtime);
    EXPECT_FALSE(parsed->config.diagnostics.progress);
    EXPECT_EQ(parsed->config.diagnostics.log_level, vcam::LogLevel::Debug);
    EXPECT_EQ(parsed->config.input.path, "video.mp4");

    Args unknown({"virtual-camera", "--colour", "red"});
    EXPECT_FALSE(vcam::parse_command_line(unknown.argc(), unknown.argv()).ok());
    Args missing({"virtual-camera", "--fps"});
    EXPECT_FALSE(vcam::parse_command_line(missing.argc(), missing.argv()).ok());
    Args help({"virtual-camera", "--help"});
    EXPECT_TRUE(vcam::parse_command_line(help.argc(), help.argv())->show_help);
    EXPECT_NE(vcam::usage_text("virtual-camera").find("playback.on_eof"), std::string::npos);
}

// Unit tests for the state machine and command parsing.
#include <gtest/gtest.h>

#include <vector>

#include "vcam/control/commands.hpp"
#include "vcam/control/state_machine.hpp"

using vcam::EngineState;

TEST(StateMachine, NormalLifecycle) {
    vcam::StateMachine machine;
    std::vector<EngineState> seen;
    machine.set_listener([&](EngineState, EngineState to) { seen.push_back(to); });
    for (EngineState next : {EngineState::Loading, EngineState::Buffering, EngineState::Ready, EngineState::Streaming,
                             EngineState::Paused, EngineState::Streaming, EngineState::Eof, EngineState::Stopping,
                             EngineState::Stopped}) {
        ASSERT_TRUE(machine.transition(next)) << vcam::state_name(next);
    }
    EXPECT_EQ(seen.size(), 9u);
    EXPECT_TRUE(vcam::is_terminal(machine.current()));
}

TEST(StateMachine, RejectsIllegalJumps) {
    vcam::StateMachine machine;
    EXPECT_FALSE(machine.transition(EngineState::Streaming));  // INIT -> STREAMING
    EXPECT_TRUE(machine.transition(EngineState::Loading));
    EXPECT_FALSE(machine.transition(EngineState::Ready));      // must buffer first
    EXPECT_TRUE(machine.transition(EngineState::Error));
    EXPECT_FALSE(machine.transition(EngineState::Stopping));   // ERROR is final
    EXPECT_EQ(machine.current(), EngineState::Error);
}

TEST(Commands, Parse) {
    EXPECT_EQ(vcam::parse_command("pause")->type, vcam::CommandType::Pause);
    EXPECT_EQ(vcam::parse_command("resume")->type, vcam::CommandType::Resume);
    EXPECT_EQ(vcam::parse_command("quit")->type, vcam::CommandType::Stop);
    auto seek = vcam::parse_command("seek 120.5");
    ASSERT_TRUE(seek.ok());
    EXPECT_EQ(seek->type, vcam::CommandType::Seek);
    EXPECT_DOUBLE_EQ(seek->seconds, 120.5);
    EXPECT_FALSE(vcam::parse_command("seek").ok());
    EXPECT_FALSE(vcam::parse_command("seek abc").ok());
    EXPECT_FALSE(vcam::parse_command("rewind").ok());
}

TEST(Commands, MailboxIsFifo) {
    vcam::CommandMailbox mailbox;
    vcam::Command out;
    EXPECT_FALSE(mailbox.take(out));
    mailbox.post({vcam::CommandType::Pause, 0});
    mailbox.post({vcam::CommandType::Seek, 3});
    ASSERT_TRUE(mailbox.take(out));
    EXPECT_EQ(out.type, vcam::CommandType::Pause);
    ASSERT_TRUE(mailbox.take(out));
    EXPECT_EQ(out.seconds, 3);
    EXPECT_FALSE(mailbox.take(out));
}

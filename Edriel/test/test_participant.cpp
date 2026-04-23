#include <gtest/gtest.h>
#include "Edriel.hpp"

using edriel::Edriel;

// Participant management tests
TEST(TestParticipant, Constructor) {
    Edriel::Participant p(123, 456, 789);
    EXPECT_EQ(p.pid, 123u);
    EXPECT_EQ(p.tid, 456u);
    EXPECT_EQ(p.uid, 789u);
}

TEST(TestParticipant, OperatorEqual) {
    Edriel::Participant p1(123, 456, 789);
    Edriel::Participant p2(123, 456, 789);
    Edriel::Participant p3(123, 456, 788);  // Different UID
    
    EXPECT_TRUE(p1 == p2);
    EXPECT_FALSE(p1 == p3);
}

TEST(TestParticipant, Timeout) {
    Edriel::Participant p(123, 456, 789);
    // Initial lastSeen should not trigger timeout
    EXPECT_FALSE(p.shouldBeRemoved());
    
    // Simulate timeout by setting lastSeen in the past
    p.lastSeen = std::chrono::steady_clock::now() - 
                  (Edriel::Participant::timeoutPeriod + std::chrono::seconds(1));
    EXPECT_TRUE(p.shouldBeRemoved());
}

TEST(TestParticipant, UpdateLastSeen) {
    Edriel::Participant p(123, 456, 789);
    auto initialTime = p.lastSeen;
    
    // Small sleep to ensure time advances
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    
    p.updateLastSeen();
    EXPECT_TRUE(p.lastSeen >= initialTime);
}

TEST(TestParticipant, TopicsInsert) {
    Edriel::Participant p(123, 456, 789);
    
    Edriel::TopicInfo t1("topic1", "MessageType1");
    Edriel::TopicInfo t2("topic2", "MessageType2");
    
    p.publishedTopics.insert(t1);
    p.publishedTopics.insert(t2);
    
    EXPECT_EQ(p.publishedTopics.size(), 2u);
}

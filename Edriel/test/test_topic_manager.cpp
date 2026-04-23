#include <gtest/gtest.h>
#include <Edriel.hpp>

// Topic Management tests
TEST(TestTopicRegistry, AddRemovePublisher) {
    TopicRegistry registry;
    
    registry.addPublisher("example/topic", 100);
    registry.addPublisher("example/topic", 200);
    
    EXPECT_TRUE(registry.hasPublisher("example/topic"));
    EXPECT_EQ(registry.getSubscribers("example/topic").empty(), true);
    
    registry.removePublisher("example/topic", 100);
    
    EXPECT_TRUE(registry.hasPublisher("example/topic"));  // Still has 200
}

TEST(TestTopicRegistry, AddRemoveSubscriber) {
    TopicRegistry registry;
    
    registry.addSubscriber("example/topic", 100);
    registry.addSubscriber("example/topic", 200);
    
    EXPECT_TRUE(registry.hasSubscriber("example/topic"));
    
    registry.removeSubscriber("example/topic", 100);
    
    EXPECT_TRUE(registry.hasSubscriber("example/topic"));  // Still has 200
}

TEST(TestTopicRegistry, PublisherSubscriberNotification) {
    TopicRegistry registry;
    
    // Add publisher
    registry.addPublisher("example/topic", 100);
    
    // Add subscriber
    registry.addSubscriber("example/topic", 200);
    
    // Verify notification happened (check subscriber set)
    EXPECT_EQ(registry.getSubscribers("example/topic").size(), 1u);
    EXPECT_TRUE(registry.getSubscribers("example/topic").count(200) == 1);
}

TEST(TestTopicRegistry, EmptyTopic) {
    TopicRegistry registry;
    
    EXPECT_FALSE(registry.hasPublisher("nonexistent"));
    EXPECT_FALSE(registry.hasSubscriber("nonexistent"));
}

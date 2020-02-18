#include "eventqueue_test.h"

void EventQueue_Test::SetUp()
{

}

void EventQueue_Test::TearDown()
{

}

TEST_F(EventQueue_Test, TopicResolve_Capacity)
{
    EventQueueCUT queue;

    for (int i = 0; i < MAX_TOPICS + 2; i++)
    {
        std::stringstream ss;
        ss << "topic_" << i;
        std::string topic = ss.str();

        int id = queue.RegisterTopic(topic);
        if (id < 0 && i < MAX_TOPICS)
        {
            FAIL() << "ID was not assigned";
        }
        else if (id < 0 && i >= MAX_TOPICS)
        {
            EXPECT_EQ(id, -2);

            EXPECT_EQ(queue.m_topicsResolveMap.size(), MAX_TOPICS);
        }
    }
}

TEST_F(EventQueue_Test, TopicResolve_Lookup)
{
    EventQueueCUT queue;

    // Populate the map
    for (int i = 0; i < MAX_TOPICS + 2; i++)
    {
        std::stringstream ss;
        ss << "topic_" << i;
        std::string topic = ss.str();

        int id = queue.RegisterTopic(topic);
        if (id < 0 && i < MAX_TOPICS)
        {
            FAIL() << "ID was not assigned";
        }
        else if (id < 0 && i >= MAX_TOPICS)
        {
            EXPECT_EQ(id, -2);

            EXPECT_EQ(queue.m_topicsResolveMap.size(), MAX_TOPICS);
        }
    }

    // Test lookup
    for (int i = 0; i < MAX_TOPICS * 2; i++)
    {
        std::stringstream ss;
        ss << "topic_" << i;
        std::string topic = ss.str();

        int id = queue.ResolveTopic(topic);
        if (i < MAX_TOPICS)
        {
            // All valid IDs within resolve array capacity should be found
            EXPECT_EQ(id, i);
        }
        else
        {
            // All others - not
            EXPECT_EQ(id, -1);
        }
    }

    // Few special cases
    std::string topic = "";
    int id = queue.ResolveTopic(topic);
    EXPECT_EQ(id, -1);

    topic = "01234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789";
    id = queue.ResolveTopic(topic);
    EXPECT_EQ(id, -1);
}
#include "messagecenter_test.h"

#include "messagecenter.h"

void MessageCenter_test::SetUp()
{

}

void MessageCenter_test::TearDown()
{

}

TEST_F(MessageCenter_test, ThreadStartStop)
{
    MessageCenterCUT& center = MessageCenterCUT::DefaultMessageCenter(false);
    center.Start();

    sleep_ms(500);

    center.Stop();

    center.dispose();

    MessageCenterCUT::DisposeDefaultMessageCenter();
}

TEST_F(MessageCenter_test, ThreadStartStopAutostart)
{
    MessageCenterCUT& center = MessageCenterCUT::DefaultMessageCenter(true);

    sleep_ms(500);

    center.Stop();

    center.dispose();

    MessageCenterCUT::DisposeDefaultMessageCenter();
}

TEST_F(MessageCenter_test, QueueOperations)
{
    MessageCenterCUT& center = MessageCenterCUT::DefaultMessageCenter(false);

    // Create topics, register lambda observer, fill queue
    FillQueue(center);


    center.Start();

    sleep_ms(1500);

    center.Stop();

    center.dispose();

    MessageCenterCUT::DisposeDefaultMessageCenter();
}
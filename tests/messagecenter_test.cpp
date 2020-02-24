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
    center.Start();

    sleep_ms(500);

    center.Stop();

    center.dispose();

    MessageCenterCUT::DisposeDefaultMessageCenter();
}
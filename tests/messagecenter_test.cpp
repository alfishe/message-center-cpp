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
    MessageCenterCUT& center = MessageCenterCUT::DefaultMessageCenter();
    center.Start();

    sleep_ms(500);

    center.Stop();

    center.dispose();
}
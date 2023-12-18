// src/common/scoped_timer_unittest.cc

#include "scoped_timer.h"
#include "gtest/gtest.h"

TEST(ScopedTimerTest, DefaultConstructor) {
	{
		util::ScopedTimer timer;
		EXPECT_EQ(timer.sayElapsed(), "elapsed time: 0s");
	}
}

TEST(ScopedTimerTest, ConstructorWithMessage) {
	{
		util::ScopedTimer timer("test message");
		EXPECT_EQ(timer.sayElapsed(), "test message: 0s");
	}
}

TEST(ScopedTimerTest, ConstructorWithTimeUnit) {
	{
		util::ScopedTimer timer(util::ScopedTimer::TimeUnit::TU_MILLI);
		EXPECT_EQ(timer.sayElapsed(), "elapsed time: 0ms");
	}
}

TEST(ScopedTimerTest, ConstructorWithMessageAndTimeUnit) {
	{
		util::ScopedTimer timer("test message",
		                        util::ScopedTimer::TimeUnit::TU_MILLI);
		EXPECT_EQ(timer.sayElapsed(), "test message: 0ms");
	}
}

TEST(ScopedTimerTest, SetTimeUnit) {
	{
		util::ScopedTimer timer;
		timer.setTimeUnit(util::ScopedTimer::TimeUnit::TU_MILLI);
		EXPECT_EQ(timer.sayElapsed(), "elapsed time: 0ms");
	}
}

// Also, ensure that the response is available in a timeframe of nanoseconds
TEST(ScopedTimerTest, TimeUnits) {
	EXPECT_EQ(static_cast<int>(util::ScopedTimer::TimeUnit::TU_SEC), 0);
	EXPECT_EQ(static_cast<int>(util::ScopedTimer::TimeUnit::TU_MILLI), 1);
	EXPECT_EQ(static_cast<int>(util::ScopedTimer::TimeUnit::TU_MICRO), 2);
	EXPECT_EQ(static_cast<int>(util::ScopedTimer::TimeUnit::TU_NANO), 3);

	{
		util::ScopedTimer timer(util::ScopedTimer::TimeUnit::TU_SEC);
		timer.setTimeUnit(util::ScopedTimer::TimeUnit::TU_SEC);
		EXPECT_EQ(timer.sayElapsed(), "elapsed time: 0s");
	}
	{
		util::ScopedTimer timer(util::ScopedTimer::TimeUnit::TU_MILLI);
		EXPECT_EQ(timer.sayElapsed(), "elapsed time: 0ms");
	}
	{
		util::ScopedTimer timer(util::ScopedTimer::TimeUnit::TU_MICRO);
		EXPECT_EQ(timer.sayElapsed(), "elapsed time: 0us");
	}
	{
		util::ScopedTimer timer(util::ScopedTimer::TimeUnit::TU_NANO);
		EXPECT_NE(timer.sayElapsed(), "elapsed time: 0ns");
		EXPECT_EQ(timer.sayElapsed().substr(0, 14), "elapsed time: ");
		EXPECT_EQ(timer.sayElapsed().substr(timer.sayElapsed().size() - 2, 2),
		          "ns");
	}
}

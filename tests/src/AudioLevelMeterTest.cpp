#include "audio/level_meter.h"
#include <gtest/gtest.h>

class AudioLevelMeterTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		audioMeter::setEnabled(false);
		audioMeter::setEnabled(true);
	}
	void TearDown() override { audioMeter::setEnabled(false); }

	void block(const audioMeter::Levels& samples, std::uint32_t token = 0)
	{
		if (!token)
			token = audioMeter::beginSample();
		for (int i = 0; i < 882; i++)
			audioMeter::record(token, samples);
	}
};

TEST_F(AudioLevelMeterTest, MeasuresBothPolaritiesAndKeepsSourcesIndependent)
{
	audioMeter::Levels samples{};
	samples[audioMeter::Output] = {-32768.f, 16384.f};
	samples[audioMeter::Cdda] = {8192.f, -4096.f};
	block(samples);
	const auto peaks = audioMeter::consume();
	EXPECT_FLOAT_EQ(peaks[audioMeter::Output][0], 1.f);
	EXPECT_FLOAT_EQ(peaks[audioMeter::Output][1], .5f);
	EXPECT_FLOAT_EQ(peaks[audioMeter::Cdda][0], .25f);
	EXPECT_FLOAT_EQ(peaks[audioMeter::Cdda][1], .125f);
	EXPECT_FLOAT_EQ(peaks[audioMeter::Aica][0], 0.f);
	EXPECT_FLOAT_EQ(peaks[audioMeter::Dsp][0], 0.f);
	EXPECT_FLOAT_EQ(peaks[audioMeter::Vmu][0], 0.f);
}

TEST_F(AudioLevelMeterTest, RetainsShortPeaksUntilConsumedAndThenClears)
{
	audioMeter::Levels impulse{};
	impulse[audioMeter::Aica] = {32768.f, 65536.f};
	audioMeter::record(audioMeter::beginSample(), impulse);
	block({});
	block({});
	const auto peaks = audioMeter::consume();
	EXPECT_FLOAT_EQ(peaks[audioMeter::Aica][0], 1.f);
	EXPECT_FLOAT_EQ(peaks[audioMeter::Aica][1], 2.f); // Pre-clipping source headroom.
	EXPECT_EQ(audioMeter::consume(), audioMeter::Levels{});
}

TEST_F(AudioLevelMeterTest, DisablingClearsPublishedAndRejectsInFlightSamples)
{
	audioMeter::Levels samples{};
	samples[audioMeter::Output] = {32768.f, 32768.f};
	const auto oldToken = audioMeter::beginSample();
	block(samples, oldToken);
	audioMeter::setEnabled(false);
	EXPECT_EQ(audioMeter::beginSample(), 0u);
	EXPECT_EQ(audioMeter::consume(), audioMeter::Levels{});
	block(samples, oldToken);
	EXPECT_EQ(audioMeter::consume(), audioMeter::Levels{});
}

TEST_F(AudioLevelMeterTest, ReopeningDropsPartialBlockFromPreviousGame)
{
	audioMeter::Levels samples{};
	samples[audioMeter::Output] = {32768.f, 32768.f};
	const auto oldToken = audioMeter::beginSample();
	audioMeter::record(oldToken, samples);
	audioMeter::setEnabled(false);
	audioMeter::setEnabled(true);
	EXPECT_NE(audioMeter::beginSample(), oldToken);
	block(samples, oldToken);
	EXPECT_EQ(audioMeter::consume(), audioMeter::Levels{});
	block({});
	EXPECT_EQ(audioMeter::consume(), audioMeter::Levels{});
}

TEST_F(AudioLevelMeterTest, RepeatedEnableDoesNotDiscardPendingAudio)
{
	audioMeter::Levels samples{};
	samples[audioMeter::Vmu] = {4096.f, 4096.f};
	const auto token = audioMeter::beginSample();
	block(samples);
	audioMeter::setEnabled(true);
	EXPECT_EQ(audioMeter::beginSample(), token);
	EXPECT_FLOAT_EQ(audioMeter::consume()[audioMeter::Vmu][0], .125f);
}

#include <array>
#include <gtest/gtest.h>

#include <moqx/stats/BoundedHistogram.h>

namespace openmoq::moqx::stats {

class BoundedHistogramTest : public ::testing::Test {
protected:
  static constexpr std::array<uint64_t, 3> bounds = {10, 100, 1000};
};

TEST_F(BoundedHistogramTest, BasicAddition) {
  BoundedHistogram<3> hist(bounds);

  hist.addValue(5);
  hist.addValue(50);
  hist.addValue(500);
  hist.addValue(5000);

  EXPECT_EQ(hist.count, 4);
  EXPECT_EQ(hist.sum, 5555);
}

TEST_F(BoundedHistogramTest, BucketDistribution) {
  BoundedHistogram<3> hist(bounds);

  hist.addValue(5);    // <= 10
  hist.addValue(10);   // <= 10
  hist.addValue(50);   // <= 100
  hist.addValue(100);  // <= 100
  hist.addValue(500);  // <= 1000
  hist.addValue(1000); // <= 1000
  hist.addValue(5000); // > 1000

  EXPECT_EQ(hist.buckets[0], 2); // <= 10
  EXPECT_EQ(hist.buckets[1], 2); // > 10 and <= 100
  EXPECT_EQ(hist.buckets[2], 2); // > 100 and <= 1000
  EXPECT_EQ(hist.buckets[3], 1); // > 1000
}

TEST_F(BoundedHistogramTest, FillCumulativeBasic) {
  BoundedHistogram<3> hist(bounds);

  hist.addValue(5);
  hist.addValue(50);
  hist.addValue(500);
  hist.addValue(5000);

  std::array<uint64_t, 4> cumulative{};
  hist.fillCumulative(cumulative);

  EXPECT_EQ(cumulative[0], 1); // <= 10
  EXPECT_EQ(cumulative[1], 2); // <= 100
  EXPECT_EQ(cumulative[2], 3); // <= 1000
  EXPECT_EQ(cumulative[3], 4); // <= +Inf (all)
}

TEST_F(BoundedHistogramTest, FillCumulativeEdgeCases) {
  BoundedHistogram<3> hist(bounds);

  // All values below first boundary
  hist.addValue(1);
  hist.addValue(5);
  hist.addValue(10);

  std::array<uint64_t, 4> cumulative{};
  hist.fillCumulative(cumulative);

  EXPECT_EQ(cumulative[0], 3);
  EXPECT_EQ(cumulative[1], 3);
  EXPECT_EQ(cumulative[2], 3);
  EXPECT_EQ(cumulative[3], 3);
}

TEST_F(BoundedHistogramTest, AllValuesAboveLastBoundary) {
  BoundedHistogram<3> hist(bounds);

  hist.addValue(10000);
  hist.addValue(20000);
  hist.addValue(30000);

  std::array<uint64_t, 4> cumulative{};
  hist.fillCumulative(cumulative);

  EXPECT_EQ(cumulative[0], 0);
  EXPECT_EQ(cumulative[1], 0);
  EXPECT_EQ(cumulative[2], 0);
  EXPECT_EQ(cumulative[3], 3);
}

TEST_F(BoundedHistogramTest, SingleBoundary) {
  constexpr std::array<uint64_t, 1> single_bound = {100};
  BoundedHistogram<1> hist(single_bound);

  hist.addValue(50);
  hist.addValue(150);

  EXPECT_EQ(hist.buckets[0], 1); // <= 100
  EXPECT_EQ(hist.buckets[1], 1); // > 100

  std::array<uint64_t, 2> cumulative{};
  hist.fillCumulative(cumulative);

  EXPECT_EQ(cumulative[0], 1);
  EXPECT_EQ(cumulative[1], 2);
}

TEST_F(BoundedHistogramTest, ZeroValues) {
  BoundedHistogram<3> hist(bounds);

  hist.addValue(0);
  hist.addValue(0);
  hist.addValue(0);

  EXPECT_EQ(hist.count, 3);
  EXPECT_EQ(hist.sum, 0);
  EXPECT_EQ(hist.buckets[0], 3); // All go to first bucket
}

} // namespace openmoq::moqx::stats

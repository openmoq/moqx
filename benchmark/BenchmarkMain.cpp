#include <folly/Benchmark.h>
#include <folly/init/Init.h>

int main(int argc, char** argv) {
  folly::Init init(&argc, &argv);
  folly::runBenchmarks();
  return 0;
}

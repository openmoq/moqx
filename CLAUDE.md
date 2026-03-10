# o-rly Development Guide

## Build, Format, Test

Always use the scripts in `scripts/` — do not invoke cmake or ctest directly.

Always run format, build, and test before committing.

```bash
scripts/format.sh          # format all source files in-place (requires clang-format-19)
scripts/build.sh           # build (handles dep setup/reconfigure automatically)
scripts/test.sh            # run all tests (ctest --output-on-failure)
```

To check formatting without modifying files:
```bash
scripts/format.sh --check
```

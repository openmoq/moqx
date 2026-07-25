# Sanitizer flags shared by the moqx build and the superbuild's moxygen san/tsan
# profiles. The two link together, so their instrumentation has to match.
set(MOQX_ASAN_FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer)
set(MOQX_TSAN_FLAGS -fsanitize=thread -fno-omit-frame-pointer)

# The dependency stack gets ASan without UBSan: folly's static_assert on syscall
# addresses (NetOps.cpp) is not constant under -fsanitize=function, which
# `,undefined` pulls in. ASan's ABI still matches across the boundary.
set(MOQX_ASAN_DEPS_FLAGS -fsanitize=address -fno-omit-frame-pointer)

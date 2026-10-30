# Gemini Code Review Guidelines for `moqx`

`moqx` is a high-performance C++20 Media over QUIC Transport (MOQT) relay built on `moxygen` (`https://github.com/openmoq/moxygen`) and `folly`, running over `mvfst` (`proxygen/quic`) and `picoquic`.

## Review Format & Output Rules

- **Skip high-level PR summary poems and walkthroughs**: CodeRabbit already generates high-level PR summaries. Keep your top-level review comment brief and focus strictly on **actionable inline findings**.
- **Prioritize high-severity defects**: Focus on correctness, memory/lifetime safety, executor/threading violations, and protocol compliance. Do not post style or formatting nitpicks already enforced by `.clang-format`, `.clang-tidy`, or `ruff.toml`.
- **Review Event (`APPROVE` vs `COMMENT`)**: Override `<SUBMIT_REVIEW>` Step 3—when calling `submit_pending_pull_request_review`, pass `event: "APPROVE"` if you found no `CRITICAL`, `HIGH`, or `MEDIUM` issues (i.e. zero inline comments or only `LOW` findings); otherwise pass `event: "COMMENT"`.

## What to Prioritize

1. **C++20 & `folly::coro` Lifetime Safety**:
   - Inspect coroutine suspension points (`co_await`, `co_yield`) for dangling references, raw pointers, or `this` captures that can outlive the owning `MoQSession`, `MoqxRelay`, `MoQForwarder`, `MoqxCache`, or `LocalForwarder` instance on session/track teardown or cancellation.
   - Check lambda captures scheduled onto `folly::Executor` / `folly::EventBase` for use-after-free hazards.

2. **Threading, Executors & Zero-Lock Data Plane (`src/relay/**`, `**/*.{h,cpp}`)**:
   - Read `docs/dev/local-forwarder-flow.md` before judging relay threading.
   - In `LocalForwarder` mode, the relay runs across multiple I/O threads with per-subscriber delivery fan-out on the publisher's and subscribers' own executors (`relayExec_` carries only a passive copy).
   - **Flag any delivery-path work that lands on `relayExec_`**, and **flag any cross-executor access that does not go through `CrossExec*` wrappers**.
   - `moqx` avoids mutexes and gratuitous atomics—including `std::shared_ptr` and `std::weak_ptr` (whose atomic refcounts contend across cores). Flag any new lock, atomic, `shared_ptr`, or `weak_ptr` unless the change strictly requires it and justifies it.

3. **MOQT & MoQ Cluster Spec Compliance (`src/**/*.{h,cpp}`)**:
   - Wire behavior must conform to both supported MOQT drafts: **draft-16** (`docs/ref/draft-ietf-moq-transport-16.txt`) and **draft-18** (`docs/ref/draft-ietf-moq-transport-18.txt`). Changes to framing, setup parameters, control messages, or error codes must remain valid across both negotiated versions.
   - Relay clustering must conform to `docs/ref/draft-lcurley-moq-cluster-01.txt` and `docs/moq-cluster.md`.

4. **Documentation, Config & Metrics Consistency**:
   - **Config (`src/config/**`)**: New or renamed fields must appear in `docs/dev/config.md`, `docs/config.md` (generated via `scripts/dev/config-schema-to-markdown.sh`), and `config.example.yaml`.
   - **Metrics (`src/{stats,admin}/**`)**: Renaming or dropping a Prometheus metric must update `docs/metrics.md`.
   - **Logging (`src/logging/**`)**: Keep new log statements on `XLOG` with an appropriate category (`docs/logging.md`).
   - **Changelog (`changelog.d/`)**: User-visible changes per `CONTRIBUTING.md#changelog` must include an entry file under `changelog.d/`, in the format of `changelog.d/README.md`.

## Paths & Files to Ignore

- Do **not** read or cite `design/**` (superseded scratchpad notes), `build/**`, `deps/**`, or `.scratch/**`.
- Treat `docs/ref/**` as read-only vendored IETF specifications (consult for normative rules using `grep_search` or targeted line ranges rather than reading an entire draft file into context; do not review for prose/style).

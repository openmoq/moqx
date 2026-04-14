# TopNFilter + PropertyRanking

The system ranks publisher tracks by a numeric property (e.g., audio level) embedded in MoQ object extensions, and delivers the top-N tracks to each subscriber. A session that is both a publisher *and* a subscriber does not receive its own tracks back — this is called **self-exclusion**.

## TopNFilter — Per-Track Observer

`TopNFilter` sits in the object delivery pipeline:

```
Publisher → TopNFilter → downstream consumer
```

It intercepts every object (`beginSubgroup`, `objectStream`, `datagram`) and calls `checkProperties()`, which scans the object's extensions for a registered property type. When it finds one:

1. **Value changed?** → fires `onValueChanged(value)`, which calls `PropertyRanking::updateSortValue()`
2. **Activity throttle passed?** → fires `onActivity()`, which triggers an idle sweep in `PropertyRanking` (bounded by `activityThreshold_` to avoid sweeping on every object)
3. **Cheap side-effect** → writes the current timestamp to `*activityTarget_` (a raw pointer to the per-track last-activity slot), costing nothing beyond a pointer write

On `publishDone`, it fires `onTrackEnded()` → `PropertyRanking::removeTrack()`.

The split between `onValueChanged` and `onActivity` matters: value updates drive ranking recomputation, while activity callbacks drive idle eviction. Both can fire on the same `checkProperties()` call.

## PropertyRanking — The Ranking Engine

`PropertyRanking` maintains a `std::map<RankKey, RankedEntry, std::greater<>>` — a descending-sorted map of all registered tracks. `RankKey` is `(value, arrivalSeq)` where higher value wins and lower `arrivalSeq` breaks ties (earlier arrivals win).

A parallel `F14FastMap<FullTrackName, RankIndex>` provides O(1) lookup by name and caches each track's integer rank.

### TopNGroups

Subscribers are grouped by their N value in `topNGroups_`. Each `TopNGroup` holds:

- `trackStates`: which tracks are `Selected` or `Deselected` (the shared top-N for viewers)
- `deselectedQueue`: a FIFO of recently-evicted tracks for cheap reselection
- `sessions`: per-session `SessionInfo`

### Rank Updates and the Fast Path

`updateSortValue()` re-inserts the track at its new key (erase + emplace in `rankedTracks_`), then:

- **Fast path**: rank move doesn't cross any group's N boundary → return immediately. This is the common case for gradual audio level drift.
- **Slow path**: calls `recomputeTopNGroups()` to update `trackStates`, fire callbacks, and promote/demote tracks.

The threshold check is O(log G) using a sorted vector of all group N values.

## Self-Exclusion

A session that publishes tracks should not receive those same tracks in its own top-N subscription. Its effective top-N is computed over the non-self subset of ranked tracks.

### Publisher Identity

`RankedEntry` stores a raw `publisherRaw` pointer (`MoQSession*`). The raw pointer is safe because `removeTrack()` is always called before session destruction (via `publishDone` → relay cleanup), and is fast for hot-path comparisons. `publisherTrackCount_` maps session pointers to track counts for O(1) `isPublisher()` checks.

### The Waterline

For a publisher-subscriber, `computeWaterlineKey()` scans `rankedTracks_` in descending order, skipping self-tracks, and returns the `RankKey` of the **Nth non-self track** — the lowest-ranked track that should still be delivered to this session.

```
rankedTracks_ (descending):
  rank 0: Alice [self]      ← skip
  rank 1: Bob               ← non-self #1
  rank 2: Carol             ← non-self #2
  rank 3: Dave              ← non-self #3 = waterline (for N=3)
  rank 4: Eve               ← below waterline, not delivered
```

If fewer than N non-self tracks exist, the waterline is `nullopt`, meaning "select all non-self tracks."

### reconcilePublisherSelection

Whenever any non-self track's rank changes (or when the session first joins), `reconcilePublisherSelection()` is called:

1. Recomputes the waterline via `computeWaterlineKey()`
2. Builds `nowSelected` = all non-self tracks at or above the waterline
3. Fires `onEvicted_` for tracks that were in `selectedTracks` but are no longer in `nowSelected`
4. Fires `onSelected_` for tracks that entered `nowSelected` but weren't in `selectedTracks`
5. Replaces `selectedTracks` with `nowSelected`

Publisher sessions do **not** use the shared `trackStates` / `deselectedQueue` — their personal selection is managed entirely through `selectedTracks` and the waterline.

### When Reconciliation Triggers

In `recomputeTopNGroups()`, for each session in a TopNGroup:

| Session type | Track that moved | Action |
|---|---|---|
| Publisher | Non-self track | `reconcilePublisherSelection()` |
| Publisher | Own track | Skip — self-track rank changes cannot shift the waterline |
| Viewer | Any | Batch `onBatchSelected_` callback |

### Late Publish: Viewer-to-Publisher Upgrade

If a session subscribes first and later publishes a track (`registerTrack()` is called while the session is already in a `TopNGroup`), `reconcilePublisherInAllGroups()` handles the transition:

1. Seeds `selectedTracks` with what the session was already receiving as a viewer, so the upcoming reconcile correctly computes the delta rather than re-notifying all tracks
2. Calls `reconcilePublisherSelection()`, which evicts the newly-self track

The session transitions seamlessly: it stops receiving its own track and continues receiving the top N non-self tracks.

## Idle Eviction

`sweepIdle()` handles tracks that are ranked into the top-N but go silent. It is triggered two ways:

- From `onActivity` on any `TopNFilter` (throttled by `sweepThrottle_`)
- Opportunistically after `updateSortValue()` completes a slow-path recompute

For each `Selected` track in each group, it calls `getLastActivity_(ftn)` (a relay callback reading the timestamp written by `TopNFilter`). If the track hasn't published within `idleTimeout_`, it is demoted via `demoteTrack()` and the highest-ranked non-selected replacement is promoted via `promoteNextAvailableTrack()`. A track that has *never* published (epoch timestamp) is treated as infinitely idle and evicted immediately.

The two-stop design — property activity fires the sweep; the sweep reads per-track timestamps — avoids an O(T) scan on every object while still detecting idle tracks promptly.

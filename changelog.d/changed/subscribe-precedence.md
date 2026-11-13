- A second SUBSCRIBE for a track the session already receives, by SUBSCRIBE or
by a PUBLISH from the relay, fails with DUPLICATE_SUBSCRIPTION, and the relay
no longer sends a PUBLISH for a track the session already subscribed to. With
draft 18, a SUBSCRIBE waiting on its rendezvous timeout gets the track when the
publisher arrives, instead of a PUBLISH through the session's SUBSCRIBE_TRACKS
or SUBSCRIBE_NAMESPACE.

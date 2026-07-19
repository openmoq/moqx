#!/usr/bin/env python3
"""
moq_decode.py — field-by-field MoQ control-stream byte annotator.

Usage:
  moq_decode.py [--version N] <hex bytes...>
  echo "16 00 12 ..." | moq_decode.py [--version N]

  --version N   draft number (default: 18)
"""

import struct
import sys

FRAME_TYPES = {
    2: "REQUEST_UPDATE",
    3: "SUBSCRIBE",
    4: "SUBSCRIBE_OK",
    5: "REQUEST_ERROR",  # SUBSCRIBE_ERROR in draft≤14
    6: "PUBLISH_NAMESPACE",
    7: "REQUEST_OK",  # PUBLISH_NAMESPACE_OK in draft≤14
    8: "NAMESPACE",  # PUBLISH_NAMESPACE_ERROR in draft≤15
    9: "PUBLISH_NAMESPACE_DONE",
    0x0A: "UNSUBSCRIBE",
    0x0B: "PUBLISH_DONE",
    0x0C: "PUBLISH_NAMESPACE_CANCEL",
    0x0D: "TRACK_STATUS",
    0x0E: "NAMESPACE_DONE",  # TRACK_STATUS_OK in draft≤15
    0x0F: "TRACK_STATUS_ERROR",
    0x10: "GOAWAY",
    0x11: "SUBSCRIBE_NAMESPACE",
    0x12: "SUBSCRIBE_NAMESPACE_OK",  # draft≤14 only
    0x13: "SUBSCRIBE_NAMESPACE_ERROR",  # draft≤14 only
    0x14: "UNSUBSCRIBE_NAMESPACE",
    0x15: "MAX_REQUEST_ID",
    0x16: "FETCH",
    0x17: "FETCH_CANCEL",
    0x18: "FETCH_OK",
    0x19: "FETCH_ERROR",
    0x1A: "REQUESTS_BLOCKED",
    0x1D: "PUBLISH",
    0x1E: "PUBLISH_OK",
    0x1F: "PUBLISH_ERROR",
    0x20: "CLIENT_SETUP",
    0x21: "SERVER_SETUP",
    0x50: "SUBSCRIBE_NAMESPACE",  # draft≥18 (renumbered from 0x11)
    0x51: "SUBSCRIBE_TRACKS",  # draft≥18
    0x2F00: "SETUP",
}

TRACK_PARAM_KEYS = {
    2: "DELIVERY_TIMEOUT",
    3: "AUTHORIZATION_TOKEN",
    4: "MAX_CACHE_DURATION",
    8: "EXPIRES",
    9: "LARGEST_OBJECT",
    0x0E: "PUBLISHER_PRIORITY",
    0x10: "FORWARD",
    0x20: "SUBSCRIBER_PRIORITY",
    0x21: "SUBSCRIPTION_FILTER",
    0x22: "GROUP_ORDER",
    0x29: "TRACK_FILTER",
    0x32: "NEW_GROUP_REQUEST",
}

SETUP_PARAM_KEYS = {
    0: "ROLE",
    1: "PATH",
    2: "MAX_REQUEST_ID",
}

GROUP_ORDER = {0: "Default", 1: "OldestFirst", 2: "NewestFirst"}
FETCH_TYPE = {1: "STANDALONE", 2: "RELATIVE_JOINING", 3: "ABSOLUTE_JOINING"}
LOCATION_TYPE = {
    1: "NextGroupStart",
    2: "LargestObject",
    3: "AbsoluteStart",
    4: "AbsoluteRange",
    250: "LargestGroup",
}


# ── low-level cursor ──────────────────────────────────────────────────────────


class ParseError(Exception):
    def __init__(self, msg, offset):
        super().__init__(msg)
        self.offset = offset


class Cursor:
    def __init__(self, data: bytes, draft: int = 18):
        self.data = data
        self.pos = 0
        self.draft = draft

    def remaining(self) -> int:
        return len(self.data) - self.pos

    def read_bytes(self, n: int) -> bytes:
        if self.pos + n > len(self.data):
            raise ParseError(
                f"underflow: need {n} byte(s), have {self.remaining()}", self.pos
            )
        result = self.data[self.pos : self.pos + n]
        self.pos += n
        return result

    def read_varint(self):
        """Returns (value, start_offset). Advances pos. Dispatches to the MoQ
        varint format on draft >= 17, else falls back to the QUIC varint."""
        if self.draft >= 17:
            return self.read_moq_varint()
        return self.read_quic_varint()

    def read_quic_varint(self):
        start = self.pos
        if self.pos >= len(self.data):
            raise ParseError("underflow reading varint", self.pos)
        first = self.data[self.pos]
        prefix = (first & 0xC0) >> 6
        if prefix == 0:
            self.pos += 1
            return first & 0x3F, start
        elif prefix == 1:
            if self.remaining() < 2:
                raise ParseError("underflow reading 2-byte varint", self.pos)
            val = ((first & 0x3F) << 8) | self.data[self.pos + 1]
            self.pos += 2
            return val, start
        elif prefix == 2:
            if self.remaining() < 4:
                raise ParseError("underflow reading 4-byte varint", self.pos)
            val = (
                ((first & 0x3F) << 24)
                | (self.data[self.pos + 1] << 16)
                | (self.data[self.pos + 2] << 8)
                | self.data[self.pos + 3]
            )
            self.pos += 4
            return val, start
        else:
            if self.remaining() < 8:
                raise ParseError("underflow reading 8-byte varint", self.pos)
            val = 0
            for i in range(8):
                val = (val << 8) | self.data[self.pos + i]
            val &= 0x00FFFFFFFFFFFFFF
            self.pos += 8
            return val, start

    def read_moq_varint(self):
        """MoQ varint (draft-17+): length is the count of leading 1 bits in
        the first byte. See MoQVarint.h.

          0xxxxxxx              -> 1 byte,  7-bit value
          10xxxxxx ...          -> 2 bytes, 14-bit value
          110xxxxx ...          -> 3 bytes, 21-bit value
          1110xxxx ...          -> 4 bytes, 28-bit value
          11110xxx ...          -> 5 bytes, 35-bit value
          111110xx ...          -> 6 bytes, 42-bit value
          1111110x ...          -> 7 bytes, 49-bit value
          11111110 ...          -> 8 bytes, 56-bit value
          11111111 <8 bytes>    -> 9 bytes, 64-bit value (byte-0 payload ignored)
        """
        start = self.pos
        if self.pos >= len(self.data):
            raise ParseError("underflow reading MoQ varint", self.pos)
        first = self.data[self.pos]
        # Count leading 1 bits in the first byte.
        leading = 0
        mask = 0x80
        while leading < 8 and (first & mask):
            leading += 1
            mask >>= 1
        # length 1..8 for leading 0..7; length 9 for leading 8.
        length = leading + 1 if leading < 8 else 9
        if self.remaining() < length:
            raise ParseError(f"underflow reading {length}-byte MoQ varint", self.pos)
        if length == 1:
            self.pos += 1
            return first, start
        if length == 9:
            val = 0
            for i in range(1, 9):
                val = (val << 8) | self.data[self.pos + i]
            self.pos += 9
            return val, start
        # length 2..8: value bits = first byte's low (8 - length) bits, then
        # (length - 1) full big-endian bytes.
        val_first_mask = (1 << (8 - length)) - 1
        val = first & val_first_mask
        for i in range(1, length):
            val = (val << 8) | self.data[self.pos + i]
        self.pos += length
        return val, start

    def read_uint16_be(self):
        """Returns (value, start_offset)."""
        start = self.pos
        return struct.unpack(">H", self.read_bytes(2))[0], start

    def read_uint8(self):
        """Returns (value, start_offset)."""
        start = self.pos
        return self.read_bytes(1)[0], start

    def read_string(self):
        """varint-length-prefixed UTF-8 string. Returns (str, start_offset)."""
        start = self.pos
        length, _ = self.read_varint()
        if self.pos + length > len(self.data):
            raise ParseError(f"underflow reading string of {length} byte(s)", self.pos)
        raw = self.read_bytes(length)
        return raw.decode("utf-8", errors="replace"), start


# ── annotation table ──────────────────────────────────────────────────────────


class Annotator:
    def __init__(self, data: bytes):
        self.data = data
        self.rows = []  # (start, end, field, value, error)
        self.error_offset = None
        self.error_msg = None

    def add(self, start: int, end: int, field: str, value: str, error: bool = False):
        self.rows.append((start, end, field, value, error))
        if error and self.error_offset is None:
            self.error_offset = start

    def _hex(self, start: int, end: int) -> str:
        return " ".join(f"{b:02x}" for b in self.data[start:end])

    def render(self):
        W_HEX = 20
        W_FIELD = 28

        print(f"\n  {'Offset':<8}  {'Bytes':<{W_HEX}}  {'Field':<{W_FIELD}}  Value")
        print(f"  {'──────':<8}  {'─────':<{W_HEX}}  {'─────':<{W_FIELD}}  ─────")

        for start, end, field, value, error in self.rows:
            hex_str = self._hex(start, end)
            if len(hex_str) > W_HEX - 1:
                hex_str = hex_str[: W_HEX - 4] + "..."
            marker = "! " if error else "  "
            print(
                f"{marker}[{start:04x}]  {hex_str:<{W_HEX}}  {field:<{W_FIELD}}  {value}"
            )

        print()

        # Annotated hex dump, 16 bytes per line
        per_line = 16
        total = len(self.data)
        print("  Hex dump:")
        for base in range(0, total, per_line):
            chunk = self.data[base : base + per_line]
            hex_parts = [f"{b:02x}" for b in chunk]
            marker_parts = ["  " for _ in chunk]

            if self.error_offset is not None:
                rel = self.error_offset - base
                if 0 <= rel < len(chunk):
                    marker_parts[rel] = "^^"

            print(f"  {base:04x}  {' '.join(hex_parts)}")
            if "^^" in marker_parts:
                print(f"        {' '.join(marker_parts)}")

        if self.error_msg:
            print(f"\n  ! Error at offset 0x{self.error_offset:02x}: {self.error_msg}")
        print()


# ── field parsers ─────────────────────────────────────────────────────────────


def p_location(cursor, annot, prefix):
    val, s = cursor.read_varint()
    annot.add(s, cursor.pos, f"{prefix}.group", str(val))
    val, s = cursor.read_varint()
    annot.add(s, cursor.pos, f"{prefix}.object", str(val))


def p_namespace(cursor, annot):
    count, s = cursor.read_varint()
    annot.add(s, cursor.pos, "namespace.count", str(count))
    for i in range(count):
        ns, s = cursor.read_string()
        annot.add(s, cursor.pos, f"namespace[{i}]", repr(ns))
    return count


def p_track_name(cursor, annot):
    name, s = cursor.read_string()
    annot.add(s, cursor.pos, "track_name", repr(name))


def p_full_track_name(cursor, annot):
    p_namespace(cursor, annot)
    p_track_name(cursor, annot)


def p_loc_type_and_location(cursor, annot, prefix, draft):
    """Reads inline loc_type + optional location (draft<15 subscribe/publish_ok format)."""
    val, s = cursor.read_varint()
    loc_name = LOCATION_TYPE.get(val, f"UNKNOWN({val})")
    annot.add(s, cursor.pos, f"{prefix}.loc_type", f"{val}  [{loc_name}]")
    if val == 3:  # AbsoluteStart
        p_location(cursor, annot, f"{prefix}.start")
    elif val == 4:  # AbsoluteRange
        p_location(cursor, annot, f"{prefix}.start")
        eg, s = cursor.read_varint()
        annot.add(s, cursor.pos, f"{prefix}.end_group", str(eg))


def p_subscription_filter_blob(raw_bytes, outer_base, annot, idx, draft):
    """Decodes the inner content of a SUBSCRIPTION_FILTER parameter blob."""
    inner = Cursor(raw_bytes, draft=draft)
    try:
        ft_val, ft_s = inner.read_varint()
        ft_name = LOCATION_TYPE.get(ft_val, f"UNKNOWN({ft_val})")
        annot.add(
            outer_base + ft_s,
            outer_base + inner.pos,
            f"param[{idx}].filter_type",
            f"{ft_val}  [{ft_name}]",
        )
        start_group = None
        if ft_val in (3, 4):  # AbsoluteStart, AbsoluteRange
            g, gs = inner.read_varint()
            start_group = g
            annot.add(
                outer_base + gs,
                outer_base + inner.pos,
                f"param[{idx}].start.group",
                str(g),
            )
            o, os_ = inner.read_varint()
            annot.add(
                outer_base + os_,
                outer_base + inner.pos,
                f"param[{idx}].start.object",
                str(o),
            )
        if ft_val == 4:  # AbsoluteRange — also has end_group
            eg, egs = inner.read_varint()
            # Draft 18+: end_group is encoded as a delta from start.group.
            if draft >= 18 and start_group is not None:
                annot.add(
                    outer_base + egs,
                    outer_base + inner.pos,
                    f"param[{idx}].end_group_delta",
                    f"{eg}  (absolute end_group={start_group + eg})",
                )
            else:
                annot.add(
                    outer_base + egs,
                    outer_base + inner.pos,
                    f"param[{idx}].end_group",
                    str(eg),
                )
    except ParseError:
        pass


def p_params(cursor, annot, draft, count, param_keys):
    prev_key = 0
    for i in range(count):
        raw_key, s = cursor.read_varint()

        # v16+: key is a delta from the previous key
        if draft >= 16:
            abs_key = prev_key + raw_key
            prev_key = abs_key
            delta_str = f" (Δ={raw_key})" if raw_key else ""
        else:
            abs_key = raw_key
            delta_str = ""

        key_name = param_keys.get(abs_key, None)
        # Draft 16+ requires the receiver to know every parameter key. In
        # draft 18 Message Parameters are Type+Value (no length envelope),
        # so unknown keys cannot be skipped — section 10.2 says receivers
        # MUST close the session with PROTOCOL_VIOLATION.
        is_unknown = draft >= 16 and key_name is None
        label = key_name if key_name else f"UNKNOWN({abs_key})"
        annot.add(
            s,
            cursor.pos,
            f"param[{i}].key",
            f"{abs_key}{delta_str}  [{label}]",
            error=is_unknown,
        )
        if is_unknown:
            raise ParseError(
                f"Unknown parameter key {abs_key} in v{draft} (delta={raw_key})", s
            )

        if abs_key == 9:  # LARGEST_OBJECT: length-prefixed AbsoluteLocation
            vlen, vs = cursor.read_varint()
            annot.add(vs, cursor.pos, f"param[{i}].length", str(vlen))
            p_location(cursor, annot, f"param[{i}].largest_object")
        elif abs_key == 0x21:  # SUBSCRIPTION_FILTER: decode blob inline
            vlen, vs = cursor.read_varint()
            annot.add(vs, cursor.pos, f"param[{i}].length", str(vlen))
            blob_start = cursor.pos
            raw = cursor.read_bytes(vlen)
            p_subscription_filter_blob(raw, blob_start, annot, i, draft)
        elif draft >= 18 and abs_key in (0x10, 0x20, 0x22):
            # d18 §10.2.7/8/12: FORWARD (0x10), SUBSCRIBER_PRIORITY (0x20) and
            # GROUP_ORDER (0x22) values are a fixed uint8, overriding the
            # generic even-key varint (§1.4.3). Diverges at value >= 64.
            val, vs = cursor.read_uint8()
            annot.add(vs, cursor.pos, f"param[{i}].value", str(val))
        elif abs_key % 2 == 0:
            # Even-keyed params are plain varints
            val, vs = cursor.read_varint()
            annot.add(vs, cursor.pos, f"param[{i}].value", str(val))
        else:
            # Odd-keyed params (and AUTH_TOKEN=3) are length-prefixed blobs
            vlen, vs = cursor.read_varint()
            annot.add(vs, cursor.pos, f"param[{i}].length", str(vlen))
            raw = cursor.read_bytes(vlen)
            annot.add(
                cursor.pos - vlen,
                cursor.pos,
                f"param[{i}].value",
                repr(raw.decode("utf-8", errors="replace")),
            )


def p_extensions(cursor, annot, payload_end):
    """Reads delta-encoded extension key-value pairs until payload_end (draft-16+)."""
    prev_type = 0
    i = 0
    while cursor.pos < payload_end:
        raw_type, s = cursor.read_varint()
        abs_type = prev_type + raw_type
        prev_type = abs_type
        delta_str = f" (Δ={raw_type})" if raw_type else ""
        if abs_type % 2 == 0:  # even: varint value
            annot.add(s, cursor.pos, f"ext[{i}].type", f"{abs_type}{delta_str}")
            val, vs = cursor.read_varint()
            annot.add(vs, cursor.pos, f"ext[{i}].value", str(val))
        else:  # odd: length-prefixed bytes
            annot.add(s, cursor.pos, f"ext[{i}].type", f"{abs_type}{delta_str}")
            vlen, vs = cursor.read_varint()
            annot.add(vs, cursor.pos, f"ext[{i}].length", str(vlen))
            raw = cursor.read_bytes(vlen)
            annot.add(
                cursor.pos - vlen,
                cursor.pos,
                f"ext[{i}].value",
                repr(raw.decode("utf-8", errors="replace")),
            )
        i += 1


# ── subscribe-request helper (SUBSCRIBE, TRACK_STATUS draft≥14) ──────────────


def p_subscribe_request_body(cursor, annot, draft):
    val, s = cursor.read_varint()
    annot.add(s, cursor.pos, "request_id", str(val))
    p_full_track_name(cursor, annot)

    if draft < 15:
        val, s = cursor.read_uint8()
        annot.add(s, cursor.pos, "priority", str(val))
        val, s = cursor.read_uint8()
        annot.add(s, cursor.pos, "group_order", GROUP_ORDER.get(val, f"UNKNOWN({val})"))
        val, s = cursor.read_uint8()
        annot.add(s, cursor.pos, "forward", str(val))
        p_loc_type_and_location(cursor, annot, "filter", draft)

    count, s = cursor.read_varint()
    annot.add(s, cursor.pos, "num_params", str(count))
    p_params(cursor, annot, draft, count, TRACK_PARAM_KEYS)


# ── per-message parsers ───────────────────────────────────────────────────────


def parse_request_update(cursor, annot, draft, payload_end):
    val, s = cursor.read_varint()
    annot.add(s, cursor.pos, "request_id", str(val))

    # d18 §10.9 (Figure 12) removed Existing Request ID from REQUEST_UPDATE;
    # d14-16 still carry it.
    if 14 <= draft < 18:
        val, s = cursor.read_varint()
        annot.add(s, cursor.pos, "existing_request_id", str(val))

    if draft < 15:
        val, s = cursor.read_varint()
        annot.add(s, cursor.pos, "start.group", str(val))
        val, s = cursor.read_varint()
        annot.add(s, cursor.pos, "start.object", str(val))
        val, s = cursor.read_varint()
        annot.add(s, cursor.pos, "end_group", str(val))
        val, s = cursor.read_uint8()
        annot.add(s, cursor.pos, "priority", str(val))
        val, s = cursor.read_uint8()
        annot.add(s, cursor.pos, "forward", str(val))

    count, s = cursor.read_varint()
    annot.add(s, cursor.pos, "num_params", str(count))
    p_params(cursor, annot, draft, count, TRACK_PARAM_KEYS)


def parse_subscribe(cursor, annot, draft, payload_end=None):
    p_subscribe_request_body(cursor, annot, draft)


def parse_subscribe_ok(cursor, annot, draft, payload_end):
    # Draft 17+: request_id is implicit from the bidi request stream context.
    if draft < 17:
        val, s = cursor.read_varint()
        annot.add(s, cursor.pos, "request_id", str(val))
    val, s = cursor.read_varint()
    annot.add(s, cursor.pos, "track_alias", str(val))

    if draft < 15:
        val, s = cursor.read_varint()
        annot.add(s, cursor.pos, "expires_ms", str(val))
        val, s = cursor.read_uint8()
        annot.add(s, cursor.pos, "group_order", GROUP_ORDER.get(val, f"UNKNOWN({val})"))

    if draft < 16:
        val, s = cursor.read_uint8()
        annot.add(s, cursor.pos, "content_exists", str(val))
        if val:
            p_location(cursor, annot, "largest")

    count, s = cursor.read_varint()
    annot.add(s, cursor.pos, "num_params", str(count))
    p_params(cursor, annot, draft, count, TRACK_PARAM_KEYS)

    if draft >= 16 and cursor.pos < payload_end:
        p_extensions(cursor, annot, payload_end)


def parse_request_error(cursor, annot, draft, payload_end):
    # Draft 17+: request_id is implicit from the bidi request stream context;
    # the receiver FIFO-correlates errors with outstanding requests.
    if draft < 17:
        val, s = cursor.read_varint()
        annot.add(s, cursor.pos, "request_id", str(val))
    val, s = cursor.read_varint()
    annot.add(s, cursor.pos, "error_code", str(val))
    if draft >= 16:
        val, s = cursor.read_varint()
        annot.add(s, cursor.pos, "retry_interval_ms", str(val))
    name, s = cursor.read_string()
    annot.add(s, cursor.pos, "reason_phrase", repr(name))


def parse_publish_namespace(cursor, annot, draft, payload_end):
    val, s = cursor.read_varint()
    annot.add(s, cursor.pos, "request_id", str(val))
    p_namespace(cursor, annot)
    count, s = cursor.read_varint()
    annot.add(s, cursor.pos, "num_params", str(count))
    p_params(cursor, annot, draft, count, TRACK_PARAM_KEYS)


def parse_request_ok(cursor, annot, draft, payload_end):
    # Draft 17+: request_id is implicit (per-stream).
    if draft < 17:
        val, s = cursor.read_varint()
        annot.add(s, cursor.pos, "request_id", str(val))
    if draft > 14:
        count, s = cursor.read_varint()
        annot.add(s, cursor.pos, "num_params", str(count))
        p_params(cursor, annot, draft, count, TRACK_PARAM_KEYS)

    # Draft 18+: any remaining bytes are Track Properties — bare KV-pair
    # extensions, no length prefix, no count, consumed until payload_end.
    # In practice the writer only emits these for TRACK_STATUS_OK responses,
    # but the parser side just looks for trailing bytes.
    if draft >= 18 and cursor.pos < payload_end:
        p_extensions(cursor, annot, payload_end)


def parse_namespace_or_publish_ns_error(cursor, annot, draft, payload_end):
    """0x08: NAMESPACE (draft≥16) or PUBLISH_NAMESPACE_ERROR (draft≤15)."""
    if draft >= 16:
        # NAMESPACE: just a namespace tuple
        p_namespace(cursor, annot)
    else:
        # PUBLISH_NAMESPACE_ERROR: request_id + error_code + reason
        parse_request_error(cursor, annot, draft, payload_end)


def parse_publish_namespace_done(cursor, annot, draft, payload_end):
    if draft >= 16:
        val, s = cursor.read_varint()
        annot.add(s, cursor.pos, "request_id", str(val))
    else:
        p_namespace(cursor, annot)


def parse_unsubscribe(cursor, annot, draft, payload_end):
    val, s = cursor.read_varint()
    annot.add(s, cursor.pos, "request_id", str(val))


def parse_publish_done(cursor, annot, draft, payload_end):
    # Draft 17+: request_id is implicit from the bidi request stream context.
    if draft < 17:
        val, s = cursor.read_varint()
        annot.add(s, cursor.pos, "request_id", str(val))
    val, s = cursor.read_varint()
    annot.add(s, cursor.pos, "status_code", str(val))
    val, s = cursor.read_varint()
    annot.add(s, cursor.pos, "stream_count", str(val))
    name, s = cursor.read_string()
    annot.add(s, cursor.pos, "reason_phrase", repr(name))


def parse_publish_namespace_cancel(cursor, annot, draft, payload_end):
    if draft >= 16:
        val, s = cursor.read_varint()
        annot.add(s, cursor.pos, "request_id", str(val))
    else:
        p_namespace(cursor, annot)
    val, s = cursor.read_varint()
    annot.add(s, cursor.pos, "error_code", str(val))
    name, s = cursor.read_string()
    annot.add(s, cursor.pos, "reason_phrase", repr(name))


def parse_track_status(cursor, annot, draft, payload_end):
    if draft >= 14:
        # Same body as SUBSCRIBE (writeSubscribeRequestHelper)
        p_subscribe_request_body(cursor, annot, draft)
    else:
        val, s = cursor.read_varint()
        annot.add(s, cursor.pos, "request_id", str(val))
        p_full_track_name(cursor, annot)
        count, s = cursor.read_varint()
        annot.add(s, cursor.pos, "num_params", str(count))
        p_params(cursor, annot, draft, count, TRACK_PARAM_KEYS)


def parse_track_status_ok_or_namespace_done(cursor, annot, draft, payload_end):
    """0x0E: NAMESPACE_DONE (draft≥16) or TRACK_STATUS_OK (draft≤15)."""
    if draft >= 16:
        # NAMESPACE_DONE: just a namespace tuple
        p_namespace(cursor, annot)
    elif draft >= 14:
        # TRACK_STATUS_OK (draft 14-15): same body as SUBSCRIBE_OK
        parse_subscribe_ok(cursor, annot, draft, payload_end)
    else:
        # TRACK_STATUS_OK (draft < 14)
        val, s = cursor.read_varint()
        annot.add(s, cursor.pos, "request_id", str(val))
        val, s = cursor.read_varint()
        annot.add(s, cursor.pos, "status_code", str(val))
        p_location(cursor, annot, "largest")
        count, s = cursor.read_varint()
        annot.add(s, cursor.pos, "num_params", str(count))
        p_params(cursor, annot, draft, count, TRACK_PARAM_KEYS)


def parse_goaway(cursor, annot, draft, payload_end):
    name, s = cursor.read_string()
    annot.add(s, cursor.pos, "new_session_uri", repr(name))
    if draft >= 18:
        val, s = cursor.read_varint()
        annot.add(s, cursor.pos, "timeout_ms", str(val))
        val, s = cursor.read_varint()
        annot.add(s, cursor.pos, "request_id", str(val))


def parse_subscribe_namespace(cursor, annot, draft, payload_end):
    val, s = cursor.read_varint()
    annot.add(s, cursor.pos, "request_id", str(val))
    p_namespace(cursor, annot)
    # The options field exists only in drafts 16-17. Draft 18 dropped it —
    # SUBSCRIBE_NAMESPACE is NAMESPACE-only, and peers wanting PUBLISH fan-out
    # use the new SUBSCRIBE_TRACKS (0x51) message instead.
    if 16 <= draft < 18:
        val, s = cursor.read_varint()
        annot.add(s, cursor.pos, "subscribe_options", str(val))
    count, s = cursor.read_varint()
    annot.add(s, cursor.pos, "num_params", str(count))
    p_params(cursor, annot, draft, count, TRACK_PARAM_KEYS)


def parse_subscribe_tracks(cursor, annot, draft, payload_end):
    """Draft 18+ only: SUBSCRIBE_TRACKS (0x51)."""
    val, s = cursor.read_varint()
    annot.add(s, cursor.pos, "request_id", str(val))
    p_namespace(cursor, annot)
    count, s = cursor.read_varint()
    annot.add(s, cursor.pos, "num_params", str(count))
    p_params(cursor, annot, draft, count, TRACK_PARAM_KEYS)


def parse_subscribe_namespace_ok(cursor, annot, draft, payload_end):
    # draft≤14 only has this type; draft≥15 sends REQUEST_OK (0x07) instead
    val, s = cursor.read_varint()
    annot.add(s, cursor.pos, "request_id", str(val))


def parse_unsubscribe_namespace(cursor, annot, draft, payload_end):
    if draft >= 15:
        val, s = cursor.read_varint()
        annot.add(s, cursor.pos, "request_id", str(val))
    else:
        p_namespace(cursor, annot)


def parse_max_request_id(cursor, annot, draft, payload_end):
    val, s = cursor.read_varint()
    annot.add(s, cursor.pos, "request_id", str(val))


def parse_fetch(cursor, annot, draft, payload_end):
    val, s = cursor.read_varint()
    annot.add(s, cursor.pos, "request_id", str(val))

    if draft < 15:
        val, s = cursor.read_uint8()
        annot.add(s, cursor.pos, "priority", str(val))
        val, s = cursor.read_uint8()
        annot.add(s, cursor.pos, "group_order", GROUP_ORDER.get(val, f"UNKNOWN({val})"))

    fetch_type, ft_s = cursor.read_varint()
    annot.add(
        ft_s,
        cursor.pos,
        "fetch_type",
        FETCH_TYPE.get(fetch_type, f"UNKNOWN({fetch_type})"),
    )

    if fetch_type == 1:  # STANDALONE
        p_full_track_name(cursor, annot)
        # In draft 18+, the end location's group is encoded as a delta from
        # start.group; the object field stays absolute. Track start.group so
        # we can render the absolute end_group too.
        sg_val, sg_s = cursor.read_varint()
        annot.add(sg_s, cursor.pos, "start.group", str(sg_val))
        so_val, so_s = cursor.read_varint()
        annot.add(so_s, cursor.pos, "start.object", str(so_val))
        if draft >= 18:
            eg_val, eg_s = cursor.read_varint()
            annot.add(
                eg_s,
                cursor.pos,
                "end.group_delta",
                f"{eg_val}  (absolute end.group={sg_val + eg_val})",
            )
            eo_val, eo_s = cursor.read_varint()
            annot.add(eo_s, cursor.pos, "end.object", str(eo_val))
        else:
            p_location(cursor, annot, "end")
    elif fetch_type in (2, 3):
        val, s = cursor.read_varint()
        annot.add(s, cursor.pos, "joining_request_id", str(val))
        val, s = cursor.read_varint()
        annot.add(s, cursor.pos, "joining_start", str(val))
    else:
        raise ParseError(f"Invalid fetch_type {fetch_type}", ft_s)

    count, s = cursor.read_varint()
    annot.add(s, cursor.pos, "num_params", str(count))
    p_params(cursor, annot, draft, count, TRACK_PARAM_KEYS)


def parse_fetch_cancel(cursor, annot, draft, payload_end):
    val, s = cursor.read_varint()
    annot.add(s, cursor.pos, "request_id", str(val))


def parse_fetch_ok(cursor, annot, draft, payload_end):
    # Draft 17+: request_id is implicit from the bidi request stream context.
    if draft < 17:
        val, s = cursor.read_varint()
        annot.add(s, cursor.pos, "request_id", str(val))

    if draft < 15:
        val, s = cursor.read_uint8()
        annot.add(s, cursor.pos, "group_order", GROUP_ORDER.get(val, f"UNKNOWN({val})"))

    val, s = cursor.read_uint8()
    annot.add(s, cursor.pos, "end_of_track", str(val))
    p_location(cursor, annot, "end")

    count, s = cursor.read_varint()
    annot.add(s, cursor.pos, "num_params", str(count))
    p_params(cursor, annot, draft, count, TRACK_PARAM_KEYS)

    if draft >= 16 and cursor.pos < payload_end:
        p_extensions(cursor, annot, payload_end)


def parse_requests_blocked(cursor, annot, draft, payload_end):
    val, s = cursor.read_varint()
    annot.add(s, cursor.pos, "max_request_id", str(val))


def parse_publish(cursor, annot, draft, payload_end):
    val, s = cursor.read_varint()
    annot.add(s, cursor.pos, "request_id", str(val))
    p_full_track_name(cursor, annot)
    val, s = cursor.read_varint()
    annot.add(s, cursor.pos, "track_alias", str(val))

    if draft < 15:
        val, s = cursor.read_uint8()
        annot.add(s, cursor.pos, "group_order", GROUP_ORDER.get(val, f"UNKNOWN({val})"))
        val, s = cursor.read_uint8()
        annot.add(s, cursor.pos, "content_exists", str(val))
        if val:
            p_location(cursor, annot, "largest")
        val, s = cursor.read_uint8()
        annot.add(s, cursor.pos, "forward", str(val))

    count, s = cursor.read_varint()
    annot.add(s, cursor.pos, "num_params", str(count))
    p_params(cursor, annot, draft, count, TRACK_PARAM_KEYS)

    if draft >= 16 and cursor.pos < payload_end:
        p_extensions(cursor, annot, payload_end)


def parse_publish_ok(cursor, annot, draft, payload_end):
    # In d18, PUBLISH_OK is just a shorthand for REQUEST_OK (0x07) in
    # response to a PUBLISH (spec section 10.5). The 0x1E wire code is not
    # used in d18 — seeing it here is a protocol violation / pre-d18 trace.
    if draft >= 18:
        annot.add(
            cursor.pos,
            cursor.pos,
            "(invalid)",
            "PUBLISH_OK (0x1E) is a REQUEST_OK alias in d18 — wire type should be 0x07",
            error=True,
        )
        raise ParseError("PUBLISH_OK (0x1E) not on the wire in draft 18", cursor.pos)
    val, s = cursor.read_varint()
    annot.add(s, cursor.pos, "request_id", str(val))

    if draft < 15:
        val, s = cursor.read_uint8()
        annot.add(s, cursor.pos, "forward", str(val))
        val, s = cursor.read_uint8()
        annot.add(s, cursor.pos, "subscriber_priority", str(val))
        val, s = cursor.read_uint8()
        annot.add(s, cursor.pos, "group_order", GROUP_ORDER.get(val, f"UNKNOWN({val})"))
        p_loc_type_and_location(cursor, annot, "filter", draft)

    count, s = cursor.read_varint()
    annot.add(s, cursor.pos, "num_params", str(count))
    p_params(cursor, annot, draft, count, TRACK_PARAM_KEYS)


def parse_client_setup(cursor, annot, draft, payload_end=None):
    # Version array only present when not negotiated via ALPN (draft < 15 compat)
    count, s = cursor.read_varint()
    annot.add(s, cursor.pos, "num_versions", str(count))
    for i in range(count):
        val, s = cursor.read_varint()
        major = val & 0xFF
        annot.add(s, cursor.pos, f"version[{i}]", f"0x{val:x}  (draft-{major})")

    count, s = cursor.read_varint()
    annot.add(s, cursor.pos, "num_params", str(count))
    p_params(cursor, annot, draft, count, SETUP_PARAM_KEYS)


def parse_server_setup(cursor, annot, draft, payload_end=None):
    val, s = cursor.read_varint()
    major = val & 0xFF
    annot.add(s, cursor.pos, "selected_version", f"0x{val:x}  (draft-{major})")

    count, s = cursor.read_varint()
    annot.add(s, cursor.pos, "num_params", str(count))
    p_params(cursor, annot, draft, count, SETUP_PARAM_KEYS)


PARSERS = {
    0x02: parse_request_update,
    0x03: parse_subscribe,
    0x04: parse_subscribe_ok,
    0x05: parse_request_error,
    0x06: parse_publish_namespace,
    0x07: parse_request_ok,
    0x08: parse_namespace_or_publish_ns_error,
    0x09: parse_publish_namespace_done,
    0x0A: parse_unsubscribe,
    0x0B: parse_publish_done,
    0x0C: parse_publish_namespace_cancel,
    0x0D: parse_track_status,
    0x0E: parse_track_status_ok_or_namespace_done,
    0x0F: parse_request_error,
    0x10: parse_goaway,
    0x11: parse_subscribe_namespace,
    0x12: parse_subscribe_namespace_ok,
    0x13: parse_request_error,
    0x14: parse_unsubscribe_namespace,
    0x15: parse_max_request_id,
    0x16: parse_fetch,
    0x17: parse_fetch_cancel,
    0x18: parse_fetch_ok,
    0x19: parse_request_error,
    0x1A: parse_requests_blocked,
    0x1D: parse_publish,
    0x1E: parse_publish_ok,
    0x1F: parse_request_error,
    0x20: parse_client_setup,
    0x21: parse_server_setup,
}


# ── main ──────────────────────────────────────────────────────────────────────


def main():
    args = sys.argv[1:]
    draft = 18
    hex_parts = []

    i = 0
    while i < len(args):
        if args[i] == "--version" and i + 1 < len(args):
            draft = int(args[i + 1])
            i += 2
        elif args[i].startswith("--version="):
            draft = int(args[i].split("=", 1)[1])
            i += 1
        else:
            hex_parts.append(args[i])
            i += 1

    if not hex_parts:
        hex_parts = sys.stdin.read().split()

    if not hex_parts:
        print(__doc__)
        sys.exit(1)

    data = bytes(int(h, 16) for h in hex_parts)

    print(f"Decoding {len(data)} bytes (draft-{draft}):")
    print(f"  {' '.join(f'{b:02x}' for b in data)}")

    annot = Annotator(data)
    cursor = Cursor(data, draft=draft)

    # In draft 18, SUBSCRIBE_NAMESPACE moved to 0x50 and SUBSCRIBE_TRACKS
    # appeared at 0x51. In drafts <18, 0x11 still resolves to SUBSCRIBE_NAMESPACE.
    parsers = dict(PARSERS)
    if draft >= 18:
        parsers[0x50] = parse_subscribe_namespace
        parsers[0x51] = parse_subscribe_tracks
        # 0x11 (LEGACY_SUBSCRIBE_NAMESPACE) shouldn't appear on the wire in
        # d18; leave it mapped so we still annotate it usefully if it does.

    try:
        # Frame type (varint — QUIC form pre-d17, MoQ form d17+)
        ftype_val, s = cursor.read_varint()
        type_name = FRAME_TYPES.get(ftype_val, f"UNKNOWN(0x{ftype_val:x})")
        annot.add(s, cursor.pos, "frame_type", f"{type_name}  (0x{ftype_val:x})")

        # Frame length (uint16 big-endian — NOT a varint)
        frame_len, s = cursor.read_uint16_be()
        annot.add(s, cursor.pos, "frame_length", str(frame_len))

        payload_start = cursor.pos
        payload_end = payload_start + frame_len

        if payload_end > len(data):
            raise ParseError(
                f"frame_length={frame_len} exceeds remaining "
                f"{len(data) - payload_start} bytes",
                s,
            )

        parser = parsers.get(ftype_val)
        if parser:
            parser(cursor, annot, draft, payload_end)
        else:
            raw = cursor.read_bytes(frame_len)
            annot.add(
                payload_start,
                cursor.pos,
                "payload",
                f"({frame_len} raw bytes — parser not implemented for {type_name})",
            )

        leftover = payload_end - cursor.pos
        if leftover > 0:
            ls = cursor.pos
            cursor.read_bytes(leftover)
            annot.add(ls, cursor.pos, "(unconsumed in frame)", f"{leftover} bytes")

        if cursor.pos < len(data):
            extra = len(data) - cursor.pos
            ls = cursor.pos
            cursor.read_bytes(extra)
            annot.add(
                ls, len(data), "(beyond this frame)", f"{extra} bytes — next frame?"
            )

    except ParseError as e:
        annot.error_offset = annot.error_offset or e.offset
        annot.error_msg = str(e)

    annot.render()


if __name__ == "__main__":
    main()

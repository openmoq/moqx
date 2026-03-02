#!/usr/bin/env bash
# Generate markdown config reference from o-rly JSON schema on stdin.
# Usage: o_rly dump-config-schema | scripts/gen-config-reference.sh
set -euo pipefail

if ! command -v jq &>/dev/null; then
  echo "Error: jq is required but not found." >&2
  exit 1
fi

SCHEMA=$(cat)

cat <<'HEADER'
# o-rly Configuration Reference

> Auto-generated from JSON schema. Do not edit manually.

HEADER

echo "$SCHEMA" | jq -r '
. as $root |

# Resolve a $ref string to its definition object.
def resolve_ref:
  if ."$ref" then
    (."$ref" | ltrimstr("#/definitions/")) as $name |
    $root.definitions[$name]
  else .
  end;

# Given an anyOf array, return the non-null variant.
def resolve_anyof:
  if .anyOf then
    [.anyOf[] | select(.type != "null")] | first
  else .
  end;

# Fully resolve a property value to an object with .properties:
# strip anyOf nullability, resolve $ref, and unwrap array items.
def resolve_prop:
  resolve_anyof | resolve_ref |
  if .type == "array" and .items then
    .items | resolve_ref
  else .
  end;

# Human-readable type string for a property.
def type_str:
  resolve_anyof |
  if .type == "array" then
    (.items | resolve_ref | .type // "object") + "[]"
  elif .type then .type
  elif ."$ref" then "object"
  else "unknown"
  end;

# Recursively emit markdown table rows for all leaf fields.
# Nested objects are flattened with dotted prefixes.
def walk_props(prefix):
  if .properties then
    .properties | to_entries | sort_by(.key)[] |
    .key as $key | .value as $prop |
    ($prop | resolve_prop) as $resolved |
    if $resolved.properties then
      $resolved | walk_props("\(prefix)\($key).")
    else
      "| `\(prefix)\($key)` | \($prop | type_str) | \($prop.description // "-") |"
    end
  else empty
  end;

# Start from the root $ref.
(."$ref" | ltrimstr("#/definitions/")) as $root_name |
.definitions[$root_name].properties | to_entries | sort_by(.key)[] |
.key as $section | .value as $prop |
($prop | resolve_prop) as $resolved |

"## \($section)\n",
"\($prop.description // "")\n",
"| Field | Type | Description |",
"| --- | --- | --- |",
if $resolved.properties then
  if $prop.type == "array" or ($prop | resolve_anyof | .type) == "array" then
    $resolved | walk_props("")
  else
    $resolved | walk_props("")
  end
else
  "| `\($section)` | \($prop | type_str) | \($prop.description // "-") |"
end,
""
'

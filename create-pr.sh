#!/bin/bash
set -e

TITLE="Refactor monolith App class into modular components"
BODY_FILE="$1"

if [ ! -f "$BODY_FILE" ]; then
    echo "Error: Body file not found: $BODY_FILE"
    exit 1
fi

# Create PR via GitHub API directly
curl -X POST \
  -H "Authorization: token $(gh auth token)" \
  -H "Accept: application/vnd.github.v3+json" \
  "https://api.github.com/repos/pondycrane/reticulem-cardputer/pulls" \
  -d "$(jq -n \
    --arg title "$TITLE" \
    --arg head "modularization-refactor" \
    --arg base "master" \
    --arg body "$(cat "$BODY_FILE")" \
    '{title: $title, head: $head, base: $base, body: $body}')" 2>/dev/null | jq -r '.html_url, .number'

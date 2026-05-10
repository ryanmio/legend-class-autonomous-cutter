#!/usr/bin/env bash
# test_28 driver. Walks gates 1-3 with a prompt between each step so the
# operator can verify the Serial output. Default IP is the boat we tested
# against on 2026-05-10; pass a different IP as $1 to override.
set -u

BOAT="${1:-192.168.1.194}"
HDR="-H Content-Type:application/json"

step() {
    echo
    echo "── $1 ──"
    read -r -p "press enter to send (or ctrl-C to abort) "
}

post() {
    local path="$1" body="$2"
    echo "POST $path  $body"
    curl -fsS -X POST "http://$BOAT$path" $HDR -d "$body" || {
        echo "(curl failed — is the boat at $BOAT reachable?)"
        exit 1
    }
    echo
}

echo "test_28 driver targeting http://$BOAT"
echo "Watch Serial @ 115200 for the PASS lines."

step "Gate 1: load 3-waypoint mission"
post /mission \
  '[{"lat":37.0001,"lon":-122.0001},{"lat":37.0002,"lon":-122.0001},{"lat":37.0002,"lon":-122.0002}]'
echo "→ Serial expected: PASS (1/3)"

step "Gate 2a: sim_gps far from WP[0]"
post /sim_gps '{"lat":37.0,"lon":-122.0}'

step "Gate 2b: sim_gps AT WP[0] — capture"
post /sim_gps '{"lat":37.0001,"lon":-122.0001}'
echo "→ Serial expected: WP 0 → 1 captured, PASS (2/3)"

step "Gate 3a: sim_gps AT WP[1] — capture"
post /sim_gps '{"lat":37.0002,"lon":-122.0001}'

step "Gate 3b: sim_gps AT WP[2] — capture + MISSION COMPLETE"
post /sim_gps '{"lat":37.0002,"lon":-122.0002}'
echo "→ Serial expected: MISSION COMPLETE, PASS (3/3), summary table"

echo
echo "Done. Three PASS lines on Serial = test_28 passes."

#!/usr/bin/env sh
set -eu
# Executes a complete matrix in a fresh result directory. A build and frozen plan are required.
project=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
dir=$(mktemp -d "$project/build-amplia2/replay-XXXXXX")
python "$project/bench/amplia/run.py" --root "$project/build-amplia2/ryzen" --out "$dir"
python -c 'import glob,json,statistics,sys; rr=[json.load(open(f)) for f in glob.glob(sys.argv[1]+"/*.json")]; assert len(rr)==100; print("METRIC tiempo_relativo="+str(statistics.geometric_mean(statistics.median(s["values"]["turi"]["ns"]/s["values"]["c"]["ns"] for s in r["samples"] if s["block"]>=0) for r in rr)))' "$dir"

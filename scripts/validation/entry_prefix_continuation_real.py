#!/usr/bin/env python3
"""Explicit continuation recipes around the unchanged frozen prefix real oracle."""
import argparse,hashlib,json,sys
from pathlib import Path
import guarded_entry_prefix_real as previous
FROZEN_PREFIX_SHA='54db29bd0a691fbf00f539374e7201d2e23935ffe41d94f8c4d0ffa820fbbf7c'
RECIPES={'A':dict(max_blocks=32,max_instructions=2048),'B':dict(max_blocks=128,max_instructions=16384)}
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--continuation-recipe',choices=RECIPES,required=True)
 a,rest=p.parse_known_args()
 assert hashlib.sha256(Path(previous.__file__).read_bytes()).hexdigest()==FROZEN_PREFIX_SHA
 original_options=previous.PREFIX_OPTIONS;argv=sys.argv
 try:
  previous.PREFIX_OPTIONS=RECIPES[a.continuation_recipe].copy()
  sys.argv=[previous.__file__]+rest
  return previous.main()
 finally:previous.PREFIX_OPTIONS=original_options;sys.argv=argv
if __name__=='__main__':raise SystemExit(main())

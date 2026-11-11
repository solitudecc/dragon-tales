#!/usr/bin/env python3
"""Explicit business-corridor option around the frozen complete-state B oracle."""
import argparse,hashlib,json,sys
from pathlib import Path
import guarded_transitions_real as previous
PREVIOUS_SHA='30b10047458d52dc5c98391b5d68cfd104ee4f7a8e392763f7ba53547b09f460'
PROFILES={
 'default':dict(max_candidates=64,max_rewrites=32,max_blocks=128,max_instructions=16384,business_corridors=1),
 'wide256':dict(max_candidates=64,max_rewrites=64,max_blocks=256,max_instructions=16384,business_corridors=1),
 'cover512':dict(max_candidates=64,max_rewrites=64,max_blocks=512,max_instructions=16384,business_corridors=1),
}
def main():
 parser=argparse.ArgumentParser(add_help=False);parser.add_argument('--business-profile',choices=PROFILES,required=True);parser.add_argument('--continuation-recipe',choices=['B'],required=True);parser.add_argument('--out',type=Path,required=True)
 args,rest=parser.parse_known_args()
 if hashlib.sha256(Path(previous.__file__).read_bytes()).hexdigest()!=PREVIOUS_SHA:raise RuntimeError('frozen terminal-transition adapter changed')
 saved_options,saved_argv=previous.OPTIONS,sys.argv;status=None
 try:
  previous.OPTIONS=PROFILES[args.business_profile].copy();sys.argv=[previous.__file__]+rest+['--continuation-recipe','B','--out',str(args.out)]
  status=previous.main();return status
 finally:
  previous.OPTIONS,sys.argv=saved_options,saved_argv
  if args.out.exists():
   (args.out/'business-corridor-activation.json').write_text(json.dumps(dict(status=status,profile=args.business_profile,options=PROFILES[args.business_profile],previous_adapter_sha256=PREVIOUS_SHA,driver_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),recipe='unchanged B prefix and three legacy rounds; unchanged five cleanup passes; one final controller with business_corridors=1; no post-controller cleanup',scope='profile budgets apply only to final controller; original GuestSSA/dispatch budgets and native comparator unchanged'),indent=2)+'\n')
if __name__=='__main__':raise SystemExit(main())

#!/usr/bin/env python3
"""Safe default: v2 controls, including frozen typed-operand regressions."""
from checked_context_audit_selftest_v2 import *
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('root',type=Path);p.add_argument('out',type=Path);p.add_argument('--regressions',required=True,type=Path);a=p.parse_args();main(a.root,a.out,a.regressions)

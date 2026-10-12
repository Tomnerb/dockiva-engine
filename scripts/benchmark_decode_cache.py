#!/usr/bin/env python3
"""Compare decode-cache off/on; memory lookup cache stays as configured."""
import sys
from benchmark_lookup_cache import main
if __name__=='__main__':
    sys.argv.extend(['--cache','decode'])
    main()

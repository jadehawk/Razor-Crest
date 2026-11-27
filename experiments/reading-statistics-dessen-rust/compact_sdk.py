"""Generate app-local out-of-line SDK wrappers; preserve the SDK ABI."""
import re
import sys
from pathlib import Path
source=Path(sys.argv[1]).read_text()
source=re.sub(r"#\[inline\]\n(pub fn dsn_)",r"#[inline(never)]\n\1",source)
Path(sys.argv[2]).write_text(source)

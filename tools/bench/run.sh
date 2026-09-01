#!/usr/bin/env bash
# Same suite on macOS or Linux.
cd "$(dirname "$0")"
python3 -c "import serial, dateutil, PIL, packaging" 2>/dev/null || \
    python3 -m pip install pyserial python-dateutil Pillow packaging requests
exec python3 bench_suite.py "$@"

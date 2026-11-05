from pathlib import Path
PROJECT=Path(__file__).resolve().parents[3]
BIN=PROJECT/'native/bin'
DATA=PROJECT/'data'
REPORTS=PROJECT/'reports'
DATA.mkdir(exist_ok=True)
REPORTS.mkdir(exist_ok=True)

#!/usr/bin/env python3
"""memory-startup.py — compact session bootstrap for Earshot_reminid.

Loads context from all memory layers with token budgets:
  1. sadie          (.sadie/sadie.db  - SQLite index of project + Obsidian vaults)
  2. serena         (.serena/memories - curated project knowledge notes)
  3. memory-bank    (.opencode/memory-bank - structured project state)
  4. harness-memory (.harness-memory  - npx harness-memory dream store)
  5. vault          (Obsidian vaults via sadie 'vault/*' categories)

Usage:
  python tools/memory-startup.py            # full (~1000 tokens)
  python tools/memory-startup.py --compact  # ultra-compact (<500 tokens)
  python tools/memory-startup.py --layer 1  # single layer (1-5)
  python tools/memory-startup.py --check    # freshness: FRESH = do nothing
"""
import sqlite3
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DB = ROOT / '.sadie' / 'sadie.db'
SERENA = ROOT / '.serena' / 'memories'
MEMORY_BANK = ROOT / '.opencode' / 'memory-bank'
HARNESS = ROOT / '.harness-memory' / 'memory.sqlite'
VAULT_RE = Path('E:/dev(dave)/GTA SA Reverse Engineering Documentation')


def layer_sadie(compact=False):
    if not DB.exists():
        print('[sadie] NO DB - run: python .sadie/sadie.py init')
        return
    con = sqlite3.connect(str(DB))
    rows = con.execute(
        "SELECT category, COUNT(*) FROM items GROUP BY category "
        "ORDER BY COUNT(*) DESC").fetchall()
    total = sum(n for _, n in rows)
    print(f'[sadie] {total} items indexed:')
    for cat, n in rows[:8]:
        print(f'    {cat}: {n}')
    if not compact:
        recent = con.execute(
            "SELECT title, category FROM items "
            "ORDER BY datetime(created_at) DESC LIMIT 5").fetchall()
        print('[sadie] recent:')
        for t, c in recent:
            print(f'    - {t} [{c}]')
    con.close()


def layer_serena(compact=False):
    if not SERENA.exists():
        print('[serena] no .serena/memories')
        return
    notes = sorted(SERENA.glob('*.md'))
    print(f'[serena] {len(notes)} memory notes:')
    for p in notes:
        first = ''
        try:
            for line in p.read_text(encoding='utf-8', errors='replace').splitlines():
                if line.strip() and not line.startswith('#'):
                    first = line.strip()[:90]
                    break
        except OSError:
            pass
        print(f'    {p.stem}: {first}' if not compact else f'    {p.stem}')


def layer_memory_bank(compact=False):
    if not MEMORY_BANK.exists():
        print('[memory-bank] no .opencode/memory-bank')
        return
    files = sorted(MEMORY_BANK.glob('*.md'))
    print(f'[memory-bank] {len(files)} files:')
    for p in files:
        if not compact:
            first = ''
            try:
                for line in p.read_text(encoding='utf-8', errors='replace').splitlines():
                    if line.strip() and not line.startswith('#'):
                        first = line.strip()[:90]
                        break
            except OSError:
                pass
            print(f'    {p.stem}: {first}')
        else:
            print(f'    {p.stem}')


def layer_harness(compact=False):
    if HARNESS.exists():
        print(f'[harness-memory] db present ({HARNESS.stat().st_size} bytes)')
    else:
        print('[harness-memory] not initialized '
              '(npx harness-memory init --db .harness-memory/memory.sqlite)')


def layer_vault(compact=False):
    if not DB.exists():
        print('[vault] sadie db missing - cannot report vault coverage')
        return
    con = sqlite3.connect(str(DB))
    n_re = con.execute(
        "SELECT COUNT(*) FROM items WHERE source_file LIKE 'vault/re:%' OR source_file LIKE 'vault/sare:%'").fetchone()[0]
    con.close()
    re_exists = 'OK' if VAULT_RE.exists() else 'MISSING'
    print(f'[vault] SA RE vault: {n_re} notes [{re_exists}]')
    if not compact and n_re == 0:
        print('         run: python .sadie/sadie_vault_sync.py')


LAYERS = [layer_sadie, layer_serena, layer_memory_bank, layer_harness, layer_vault]


def check_fresh():
    """Accuracy guard: is sadie.db in sync with serena notes + vaults?"""
    import time
    db = ROOT / '.sadie' / 'sadie.db'
    if not db.exists():
        print('[STALE] sadie.db missing - run: python .sadie/sadie.py init')
        return
    db_m = db.stat().st_mtime
    serena = sorted((ROOT / '.serena' / 'memories').glob('*.md')) if (ROOT / '.serena' / 'memories').exists() else []
    stale_serena = [p.name for p in serena if p.stat().st_mtime > db_m]

    # Check vault
    vault_newer = 0
    for vault_path in [VAULT_RE]:
        if vault_path.exists():
            vault_mds = [p for p in vault_path.rglob('*.md')
                         if not any(part.startswith('.') for part in p.parts)]
            vault_newer += sum(1 for p in vault_mds if p.stat().st_mtime > db_m)

    if stale_serena:
        print(f'[STALE] serena notes newer than sadie.db: {", ".join(stale_serena)} '
              f'- run: python .sadie/sadie.py init')
        return
    if vault_newer:
        print(f'[STALE] {vault_newer} vault notes newer than sadie.db '
              f'- run: python .sadie/sadie_vault_sync.py')
        return
    print('[FRESH] all layers in sync - nothing to do')


def main():
    argv = sys.argv[1:]
    if '--check' in argv:
        check_fresh()
        return
    compact = '--compact' in argv
    single = None
    if '--layer' in argv:
        single = int(argv[argv.index('--layer') + 1])
    print('=== Earshot_reminid :: memory startup ===')
    for i, fn in enumerate(LAYERS, 1):
        if single and i != single:
            continue
        fn(compact)
    print('=== bootstrap complete ===')


if __name__ == '__main__':
    main()

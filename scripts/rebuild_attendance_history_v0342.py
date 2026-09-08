#!/usr/bin/env python3
"""One-time historical attendance rebuild for Monk_SKUD_UNEX v0.3.42.

Policy reproduced for every completed local calendar day:
  * the first accepted user-card read after 00:00 is Arrival;
  * same-day reads inside attendance.accidental_repeat_seconds are Accidental;
  * other same-day reads toggle Arrival/Departure;
  * if the employee is still Present at day end, a synthetic Departure is
    written at the timestamp of the last physical card read of that day.

The script only edits historical MariaDB rows and never runs automatically.
Preview is the default. Use --yes to apply. Current day is excluded by default.
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import os
from pathlib import Path
import subprocess
import sys
from collections import defaultdict

MARKER = "attendance_history_daily_rebuild_v0342"
AUTO_PREFIX = "AUTO_MIDNIGHT_RESET|"
SERVICE = "monk-skud-unex"


def parse_conf(path: Path) -> dict[str, str]:
    cfg: dict[str, str] = {}
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        k, v = line.split("=", 1)
        cfg[k.strip()] = v.strip()
    return cfg


def parse_stamp(value: str) -> dt.datetime:
    return dt.datetime.strptime(value[:19], "%Y-%m-%d %H:%M:%S")


def valid_date(value: str) -> str:
    dt.date.fromisoformat(value)
    return value


def decode_hex(value: str) -> str:
    if not value:
        return ""
    return bytes.fromhex(value).decode("utf-8", errors="replace")


def sql_utf8(value: str) -> str:
    if value == "":
        return "''"
    return "CONVERT(0x%s USING utf8mb4)" % value.encode("utf-8").hex()


def run_mysql(args: list[str], env: dict[str, str], sql: str, capture: bool = True) -> str:
    p = subprocess.run(
        args,
        input=sql,
        text=True,
        env=env,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.PIPE,
    )
    if p.returncode != 0:
        err = (p.stderr or "").strip()
        raise RuntimeError(err or f"mariadb exited with {p.returncode}")
    return p.stdout if capture else ""


def fetch_rows(mysql: list[str], env: dict[str, str], date_from: str, date_to: str):
    sql = f"""
SELECT id,timestamp,type,HEX(card),user_id,HEX(user_name),HEX(department),controller_node,HEX(controller_name),HEX(raw_hex)
FROM skud_attendance_events
WHERE user_id>0
  AND timestamp >= '{date_from} 00:00:00'
  AND timestamp <= '{date_to} 23:59:59'
  AND type IN ('arrival','departure','accidental')
ORDER BY user_id,timestamp,id;
"""
    out = run_mysql(mysql, env, sql)
    rows = []
    for line in out.splitlines():
        if not line.strip():
            continue
        c = line.split("\t")
        if len(c) != 10:
            raise RuntimeError(f"unexpected MariaDB row with {len(c)} columns")
        raw_hex = decode_hex(c[9])
        rows.append({
            "id": int(c[0]), "timestamp": c[1], "type": c[2],
            "card": decode_hex(c[3]), "user_id": int(c[4]),
            "user_name": decode_hex(c[5]), "department": decode_hex(c[6]),
            "controller_node": int(c[7] or 0),
            "controller_name": decode_hex(c[8]), "raw_hex": raw_hex,
            "synthetic": raw_hex.startswith(AUTO_PREFIX),
        })
    return rows


def classify(rows, repeat_seconds: int):
    physical = [r for r in rows if not r["synthetic"]]
    groups = defaultdict(list)
    for r in physical:
        groups[(r["timestamp"][:10], r["user_id"])].append(r)

    updates: list[tuple[int, str, str]] = []
    synthetics: list[dict] = []
    first_fixed = 0
    group_stats = []

    for (day, user_id), items in sorted(groups.items()):
        items.sort(key=lambda r: (r["timestamp"], r["id"]))
        present = False
        last_time = None
        planned = []
        for idx, r in enumerate(items):
            current = parse_stamp(r["timestamp"])
            accidental = False
            if last_time is not None:
                sec = int((current - last_time).total_seconds())
                accidental = 0 <= sec <= repeat_seconds
            if accidental:
                new_type = "accidental"
            elif not present:
                new_type = "arrival"
                present = True
            else:
                new_type = "departure"
                present = False
            if idx == 0 and new_type == "arrival" and r["type"] != "arrival":
                first_fixed += 1
            if r["type"] != new_type:
                updates.append((r["id"], r["type"], new_type))
            planned.append(new_type)
            last_time = current  # every physical read refreshes repeat-window base

        if present and items:
            last = items[-1]
            synthetics.append({
                "day": day, "timestamp": last["timestamp"], "card": last["card"],
                "user_id": user_id, "user_name": last["user_name"],
                "department": last["department"], "controller_node": last["controller_node"],
                "controller_name": "Автозакрытие дня 00:00",
                "raw_hex": f"AUTO_MIDNIGHT_RESET|U:{user_id}|D:{day}",
            })
        group_stats.append((day, user_id, len(items), present, planned[0] if planned else ""))
    return updates, synthetics, group_stats, first_fixed


def self_test() -> None:
    rows = [
        {"id": 1, "timestamp": "2026-09-07 23:59:58", "type": "departure", "card": "1:1", "user_id": 1, "user_name": "A", "department": "", "controller_node": 2, "controller_name": "C", "raw_hex": "R1", "synthetic": False},
        {"id": 2, "timestamp": "2026-09-08 00:00:01", "type": "departure", "card": "1:1", "user_id": 1, "user_name": "A", "department": "", "controller_node": 2, "controller_name": "C", "raw_hex": "R2", "synthetic": False},
        {"id": 3, "timestamp": "2026-09-08 00:00:03", "type": "departure", "card": "1:1", "user_id": 1, "user_name": "A", "department": "", "controller_node": 2, "controller_name": "C", "raw_hex": "R3", "synthetic": False},
        {"id": 4, "timestamp": "2026-09-08 12:00:00", "type": "arrival", "card": "1:1", "user_id": 1, "user_name": "A", "department": "", "controller_node": 2, "controller_name": "C", "raw_hex": "R4", "synthetic": False},
        {"id": 5, "timestamp": "2026-09-08 13:00:00", "type": "departure", "card": "1:1", "user_id": 1, "user_name": "A", "department": "", "controller_node": 2, "controller_name": "C", "raw_hex": "R5", "synthetic": False},
    ]
    updates, synth, stats, first_fixed = classify(rows, 5)
    resulting = {r["id"]: r["type"] for r in rows}
    for rid, _, typ in updates:
        resulting[rid] = typ
    assert resulting[1] == "arrival"
    assert resulting[2] == "arrival", "first use after midnight must be arrival"
    assert resulting[3] == "accidental", "same-day repeat must remain accidental"
    assert resulting[4] == "departure"
    assert resulting[5] == "arrival"
    assert len(synth) == 2  # day 7 and day 8 both finish Present
    assert synth[1]["timestamp"] == "2026-09-08 13:00:00"
    assert first_fixed >= 2
    print("SELF-TEST PASS: day reset, first arrival, repeat filter, midnight auto-departure")


def main() -> int:
    ap = argparse.ArgumentParser(description="One-time historical attendance rebuild for v0.3.42")
    ap.add_argument("--root", default=str(Path(__file__).resolve().parents[1]))
    ap.add_argument("--from", dest="date_from", help="first date YYYY-MM-DD (default: earliest history)")
    ap.add_argument("--to", dest="date_to", help="last date YYYY-MM-DD (default: yesterday)")
    ap.add_argument("--yes", action="store_true", help="apply changes; without this option only preview")
    ap.add_argument("--force", action="store_true", help="allow rerun after the one-time marker exists")
    ap.add_argument("--self-test", action="store_true")
    ns = ap.parse_args()
    if ns.self_test:
        self_test()
        return 0

    root = Path(ns.root).resolve()
    conf = root / "config" / "system.conf"
    if not conf.is_file():
        raise RuntimeError(f"config not found: {conf}")
    cfg = parse_conf(conf)
    if cfg.get("database.enabled", "false").lower() not in ("1", "true", "yes", "on"):
        raise RuntimeError("database.enabled is not true; this history script is intentionally MariaDB-only")

    db_name = cfg.get("database.name", "monk_skud_unex")
    if not db_name.replace("_", "").isalnum():
        raise RuntimeError("unsafe database.name")
    mysql = [
        "mariadb", "--batch", "--raw", "--skip-column-names", "--protocol=tcp",
        "-h", cfg.get("database.host", "127.0.0.1"),
        "-P", cfg.get("database.port", "3306"),
        "-u", cfg.get("database.user", "monk_skud"), db_name,
    ]
    env = os.environ.copy()
    env["MYSQL_PWD"] = cfg.get("database.password", "")
    try:
        subprocess.run(["mariadb", "--version"], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except Exception as exc:
        raise RuntimeError("mariadb client not found") from exc

    bounds = run_mysql(mysql, env, "SELECT COALESCE(MIN(LEFT(timestamp,10)),''),COALESCE(MAX(LEFT(timestamp,10)),'') FROM skud_attendance_events WHERE user_id>0 AND type IN ('arrival','departure','accidental');").strip().split("\t")
    if len(bounds) < 2 or not bounds[0]:
        print("No attendance history rows to process.")
        return 0

    yesterday = (dt.date.today() - dt.timedelta(days=1)).isoformat()
    date_from = valid_date(ns.date_from or bounds[0])
    date_to = valid_date(ns.date_to or min(bounds[1], yesterday))
    today = dt.date.today().isoformat()
    if date_to >= today:
        raise RuntimeError(f"historical rebuild must end before today ({today}); requested --to {date_to}")
    if date_from > date_to:
        print(f"Nothing to process: {date_from} > {date_to}")
        return 0

    marker = run_mysql(mysql, env, f"SELECT meta_value FROM skud_meta WHERE meta_key='{MARKER}' LIMIT 1;").strip()
    if marker and not ns.force:
        print(f"One-time marker already exists: {MARKER}={marker}")
        print("No changes made. Use --force only if you intentionally want to rebuild the range again.")
        return 2

    repeat_seconds = int(cfg.get("attendance.accidental_repeat_seconds", "5") or "5")
    rows = fetch_rows(mysql, env, date_from, date_to)
    existing_synth = [r for r in rows if r["synthetic"]]
    updates, synthetics, group_stats, first_fixed = classify(rows, repeat_seconds)
    physical_count = sum(1 for r in rows if not r["synthetic"])

    print("Monk_SKUD_UNEX v0.3.42 historical attendance rebuild")
    print(f"Range: {date_from} .. {date_to} (current day is not processed by default)")
    print(f"Repeat window: {repeat_seconds} s")
    print(f"Physical known-user card reads: {physical_count}")
    print(f"User/day groups: {len(group_stats)}")
    print(f"Rows whose type will change: {len(updates)}")
    print(f"First reads corrected to ARRIVAL: {first_fixed}")
    print(f"Old AUTO_MIDNIGHT_RESET rows to replace: {len(existing_synth)}")
    print(f"Synthetic end-of-day departures to create: {len(synthetics)}")

    if not ns.yes:
        print("\nPREVIEW ONLY. Database was not changed.")
        print("Apply with the service stopped:")
        print(f"  sudo systemctl stop {SERVICE}")
        print(f"  python3 {Path(__file__).name} --root '{root}' --yes")
        print(f"  sudo systemctl start {SERVICE}")
        return 0

    active = subprocess.run(["systemctl", "is-active", "--quiet", SERVICE]).returncode == 0
    if active:
        raise RuntimeError(f"{SERVICE} is active. Stop it before applying: sudo systemctl stop {SERVICE}")

    stamp = dt.datetime.now().strftime("%Y%m%d_%H%M%S")
    backup_table = f"skud_attendance_events_backup_v0342_{stamp}"
    print(f"Creating range backup table: {backup_table}")
    backup_sql = f"""
CREATE TABLE `{backup_table}` LIKE skud_attendance_events;
INSERT INTO `{backup_table}` SELECT * FROM skud_attendance_events
 WHERE timestamp >= '{date_from} 00:00:00' AND timestamp <= '{date_to} 23:59:59';
"""
    run_mysql(mysql, env, backup_sql)

    statements = ["START TRANSACTION;"]
    # Remove old synthetic closures in the selected range; they are rebuilt
    # deterministically after the physical card-read sequence is reclassified.
    statements.append(
        "DELETE FROM skud_attendance_events "
        f"WHERE timestamp >= '{date_from} 00:00:00' AND timestamp <= '{date_to} 23:59:59' "
        f"AND user_id>0 AND raw_hex LIKE '{AUTO_PREFIX}%';"
    )
    for rid, _old, new in updates:
        statements.append(f"UPDATE skud_attendance_events SET type='{new}' WHERE id={rid};")
    for e in synthetics:
        source = hashlib.sha256(("history-v0342|%s|%s|%s" % (e["user_id"], e["day"], e["timestamp"])).encode()).hexdigest()
        statements.append(
            "INSERT INTO skud_attendance_events(source_key,timestamp,type,card,user_id,user_name,department,controller_node,controller_name,raw_hex) VALUES("
            f"'{source}','{e['timestamp']}','departure',{sql_utf8(e['card'])},{e['user_id']},{sql_utf8(e['user_name'])},{sql_utf8(e['department'])},{e['controller_node']},{sql_utf8(e['controller_name'])},{sql_utf8(e['raw_hex'])});"
        )
    marker_value = f"applied={dt.datetime.now().isoformat(timespec='seconds')};range={date_from}..{date_to};repeat={repeat_seconds};backup={backup_table}"
    statements.append(
        f"INSERT INTO skud_meta(meta_key,meta_value) VALUES('{MARKER}',{sql_utf8(marker_value)}) "
        "ON DUPLICATE KEY UPDATE meta_value=VALUES(meta_value);"
    )
    statements.append("COMMIT;")
    run_mysql(mysql, env, "\n".join(statements))

    # Verification: physical row classifications must now match the same
    # simulation, and synthetic closure count must equal the planned count.
    after = fetch_rows(mysql, env, date_from, date_to)
    expected = {r["id"]: r["type"] for r in rows if not r["synthetic"]}
    for rid, _old, new in updates:
        expected[rid] = new
    actual = {r["id"]: r["type"] for r in after if not r["synthetic"] and r["id"] in expected}
    bad = [rid for rid, typ in expected.items() if actual.get(rid) != typ]
    synth_after = [r for r in after if r["synthetic"]]
    if bad or len(synth_after) != len(synthetics):
        raise RuntimeError(
            f"verification failed: bad physical rows={len(bad)}, synthetic={len(synth_after)}/{len(synthetics)}. "
            f"Backup table: {backup_table}"
        )

    print("APPLY PASS")
    print(f"Backup table: {backup_table}")
    print(f"Updated physical row types: {len(updates)}")
    print(f"Created synthetic departures: {len(synthetics)}")
    print(f"Marker: {MARKER}")
    print(f"Start service: sudo systemctl start {SERVICE}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        raise SystemExit(130)
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)

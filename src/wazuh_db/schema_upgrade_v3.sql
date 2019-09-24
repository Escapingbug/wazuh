/*
 * SQL Schema for upgrading databases
 * Copyright (C) 2015-2019, Wazuh Inc.
 * This program is a free software, you can redistribute it
 * and/or modify it under the terms of GPLv2.
*/

DROP TABLE IF EXISTS fim_entry;

CREATE TABLE fim_entry (
    file TEXT PRIMARY KEY,
    type TEXT NOT NULL CHECK (type IN ('file', 'registry')),
    date INTEGER NOT NULL DEFAULT (strftime('%s', 'now')),
    changes INTEGER NOT NULL DEFAULT 1,
    size INTEGER,
    perm TEXT,
    uid TEXT,
    gid TEXT,
    md5 TEXT,
    sha1 TEXT,
    uname TEXT,
    gname TEXT,
    mtime INTEGER,
    inode INTEGER,
    sha256 TEXT,
    attributes TEXT,
    symbolic_path TEXT,
    checksum TEXT
);

CREATE TABLE IF NOT EXISTS sync_info (
    component TEXT PRIMARY KEY,
    last_attempt INTEGER DEFAULT 0,
    last_completion INTEGER DEFAULT 0,
    n_attempts INTEGER DEFAULT 0,
    n_completions INTEGER DEFAULT 0
);

INSERT OR REPLACE INTO metadata (key, value) VALUES ('db_version', 3);
INSERT INTO sync_info (component) VALUES ('fim');

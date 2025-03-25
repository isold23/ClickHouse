-- Tags: no-fasttest, no-parallel

DROP TABLE IF EXISTS t1 SYNC;

CREATE TABLE t1 (x UInt32) ENGINE = ReplicatedMergeTree('/clickhouse/tables/{database}/test_03356/t1', '1') ORDER BY tuple();

SYSTEM STOP PULLING REPLICATION LOG t1;

INSERT INTO t1 VALUES (1);

SYSTEM START PULLING REPLICATION LOG t1;

ALTER TABLE t1 DETACH PART 'all_0_0_0';

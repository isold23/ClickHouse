-- Tags: no-random-settings

DROP TABLE IF EXISTS test;
CREATE TABLE test
(
    `x` Int64,
    INDEX idx1 CAST(x, 'String') TYPE bloom_filter GRANULARITY 1,
)
ENGINE = MergeTree
ORDER BY tuple();
INSERT INTO test SELECT number FROM numbers(1000000);
EXPLAIN indexes = 1 SELECT * FROM test WHERE CAST(x, 'String') = '100';
DROP TABLE test;

CREATE TABLE test
(
    `x` Int64,
    INDEX idx1 CAST(x, 'String') TYPE set(0) GRANULARITY 1,
)
ENGINE = MergeTree
ORDER BY tuple();
INSERT INTO test SELECT number FROM numbers(1000000);
EXPLAIN indexes = 1 SELECT * FROM test WHERE CAST(x, 'String') = '100';
DROP TABLE test;

CREATE TABLE test
(
    `x` Int64,
    INDEX idx1 CAST(x, 'String') TYPE tokenbf_v1(16000, 2, 0) GRANULARITY 1,
)
ENGINE = MergeTree
ORDER BY tuple();
INSERT INTO test SELECT number FROM numbers(1000000);
EXPLAIN indexes = 1 SELECT * FROM test WHERE CAST(x, 'String') = '100';
DROP TABLE test;

CREATE TABLE test
(
    `x` Int64,
    INDEX idx1 CAST(x, 'String') TYPE ngrambf_v1(4, 16000, 2, 0)  GRANULARITY 1,
)
ENGINE = MergeTree
ORDER BY tuple();
INSERT INTO test SELECT number FROM numbers(1000000);
EXPLAIN indexes = 1 SELECT * FROM test WHERE CAST(x, 'String') = '100';
DROP TABLE test;

CREATE TABLE test
(
    `x` Int64,
    INDEX idx1 CAST(x, 'String') TYPE minmax GRANULARITY 1,
)
ENGINE = MergeTree
ORDER BY tuple();
INSERT INTO test SELECT number FROM numbers(1000000);
EXPLAIN indexes = 1 SELECT * FROM test WHERE CAST(x, 'String') = '100';
DROP TABLE test;


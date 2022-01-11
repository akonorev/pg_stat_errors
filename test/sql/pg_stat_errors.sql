DROP EXTENSION pg_stat_errors;
CREATE EXTENSION pg_stat_errors;

-- first make sure that catcache is loaded to avoid physical reads
SELECT count(*) >= 0 FROM pg_stat_errors;
SELECT pg_stat_errors_reset();

-- dummy query
SELECT 1 AS dummy;

SELECT * FROM pg_stat_errors;

-- syntax error
CREATE TABLE t1 (int);

CREATE TABLE t1 (n int primary key);
CREATE TABLE t2 (n int primary key, p int references t1, dat timestamp);

-- primary key violation (15 errors) 
INSERT INTO t1 VALUES (1);
INSERT INTO t1 VALUES (1);
INSERT INTO t1 VALUES (1);
INSERT INTO t1 VALUES (1);
INSERT INTO t1 VALUES (1);
INSERT INTO t1 VALUES (1);
INSERT INTO t1 VALUES (1);
INSERT INTO t1 VALUES (1);
INSERT INTO t1 VALUES (1);
INSERT INTO t1 VALUES (1);
INSERT INTO t1 VALUES (1);
INSERT INTO t1 VALUES (1);
INSERT INTO t1 VALUES (1);
INSERT INTO t1 VALUES (1);
INSERT INTO t1 VALUES (1);
INSERT INTO t1 VALUES (1);

-- version 1.1 (overlap test. step 1)
SELECT usename, datname, error_level, error_state, error_message FROM dba_stat_errors_last
 WHERE datname = current_database()
-- ORDER BY error_time
;

-- foreign key violation
INSERT INTO t2 VALUES (1,2,null);

-- date/time field value out of range
INSERT INTO t2 VALUES (1,1,'20210931');

-- invalid input syntax for integer
INSERT INTO t1 VALUES ('test');

-- relation already exists
CREATE TABLE t1 (n int primary key);

-- relation does not exist
SELECT n FROM t3;

-- column does not exist
SELECT p from t1;

-- prepared statement does not exist
DEALLOCATE pdo_stmt_00000001;

-- version 1.1 (overlap test. step 2)
SELECT usename, datname, error_level, error_state, error_message FROM dba_stat_errors_last
 WHERE datname = current_database()
-- ORDER BY error_time
;

-- version 1.1 (overlap test. step 3)
SELECT usename, datname, error_level, error_state, error_message FROM dba_stat_errors_last
 WHERE datname = current_database()
 ORDER BY error_time
;

-- raise exception. ERROR
DO $$
BEGIN
RAISE EXCEPTION 'Test raise exception';
END;
$$
;

-- raise warning. WARNING
DO $$
BEGIN
RAISE WARNING 'Test raise warning';
END;
$$
;

SELECT usename, datname, error_level, error_class, error_state, errors FROM dba_stat_errors 
 WHERE datname = current_database() 
 ORDER BY error_state, errors;


SELECT pg_stat_errors_total_errors();

SELECT dealloc FROM pg_stat_errors_info;

DROP TABLE t2;
DROP TABLE t1;
DROP EXTENSION pg_stat_errors;


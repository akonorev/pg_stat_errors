/* pg_stat_errors/pg_stat_errors--1.2.sql */

\echo Use "CREATE EXTENSION pg_stat_errors" to load this file. \quit

-- Register functions.
CREATE FUNCTION pg_stat_errors_reset()
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C;


/* pg_stat_errors_total_errors */
CREATE FUNCTION pg_stat_errors_total_errors()
RETURNS BIGINT
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT VOLATILE;

CREATE VIEW pg_stat_errors_total_errors AS
  SELECT * FROM pg_stat_errors_total_errors();

GRANT SELECT ON pg_stat_errors_total_errors TO PUBLIC;


/* pg_stat_errors_info */
CREATE FUNCTION pg_stat_errors_info(
    OUT dealloc               bigint,
    OUT stats_reset           timestamp with time zone
)
RETURNS record
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT VOLATILE;

CREATE VIEW pg_stat_errors_info AS
  SELECT * FROM pg_stat_errors_info();

GRANT SELECT ON pg_stat_errors_info TO PUBLIC;


/* pg_stat_errors */
CREATE FUNCTION pg_stat_errors(
    OUT userid              oid,
    OUT dbid                oid,
    OUT error_level         text,
    OUT error_class         text,
    OUT error_class_message text,
    OUT error_state         text,
    OUT error_state_message text,
    ouT errors              bigint,
    OUT last_time           timestamp with time zone
)
RETURNS SETOF record
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT VOLATILE;

CREATE VIEW pg_stat_errors AS
  SELECT * FROM pg_stat_errors();

GRANT SELECT ON pg_stat_errors TO PUBLIC;


/* dba_stat_errors */
CREATE VIEW dba_stat_errors AS
SELECT 
    userid,
    ( SELECT pg_user.usename
        FROM pg_user
       WHERE pg_user.usesysid = pg_stat_errors.userid) AS usename,
    dbid,
    ( SELECT pg_database.datname
        FROM pg_database
       WHERE pg_database.oid = pg_stat_errors.dbid) AS datname,
    error_level,
    error_class,
    error_class_message,
    error_state,
    error_state_message,
    errors,
    last_time
FROM pg_stat_errors;

GRANT SELECT ON dba_stat_errors TO PUBLIC;


/* pg_stat_errors_last */
CREATE FUNCTION pg_stat_errors_last(
    OUT error_time          timestamp with time zone,
    OUT userid              oid,
    OUT dbid                oid,
    ouT query               text,
    OUT error_level         text,
    OUT error_state         text,
    ouT error_message       text
)
RETURNS SETOF record
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT VOLATILE;

CREATE VIEW pg_stat_errors_last AS
  SELECT * FROM pg_stat_errors_last();

GRANT SELECT ON pg_stat_errors_last TO PUBLIC;


/* dba_stat_errors_last */
CREATE VIEW dba_stat_errors_last AS
SELECT
    error_time,
    userid,
    ( SELECT pg_user.usename
        FROM pg_user
       WHERE pg_user.usesysid = pg_stat_errors_last.userid) AS usename,
    dbid,
    ( SELECT pg_database.datname
        FROM pg_database
       WHERE pg_database.oid = pg_stat_errors_last.dbid) AS datname,
    query,
    error_level,
    error_state,
    error_message
FROM pg_stat_errors_last;

GRANT SELECT ON dba_stat_errors_last TO PUBLIC;


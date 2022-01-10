/* pg_stat_errors/pg_stat_errors--1.0--1.1.sql */
  
-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pg_stat_errors UPDATE TO '1.1'" to load this file. \quit

/* pg_stat_errors_last */
CREATE FUNCTION pg_stat_errors_last(
    OUT error_time          timestamp with time zone,
    OUT userid              oid,
    OUT dbid                oid,
    ouT query               text,
    OUT elevel              bigint,
    OUT ecode               bigint,
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
    pg_stat_errors_glevel(elevel) AS error_level,
    pg_stat_errors_gcode(ecode) AS error_state,
    error_message
FROM pg_stat_errors_last;

GRANT SELECT ON dba_stat_errors_last TO PUBLIC;


/* pg_stat_errors/pg_stat_errors--1.1--1.2.sql */
  
-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pg_stat_errors UPDATE TO '1.2'" to load this file. \quit

/* pg_stat_errors/dba_stat_errors */

/* First we have to remove them from the extension */
ALTER EXTENSION pg_stat_errors DROP VIEW dba_stat_errors;
ALTER EXTENSION pg_stat_errors DROP VIEW pg_stat_errors;
ALTER EXTENSION pg_stat_errors DROP FUNCTION pg_stat_errors();

/* Then we can drop them */
DROP VIEW dba_stat_errors;
DROP VIEW pg_stat_errors;
DROP FUNCTION pg_stat_errors;

/* Now redefine */
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


/* pg_stat_errors_last/dba_stat_errors_last */

/* First we have to remove them from the extension */
ALTER EXTENSION pg_stat_errors DROP VIEW dba_stat_errors_last;
ALTER EXTENSION pg_stat_errors DROP VIEW pg_stat_errors_last;
ALTER EXTENSION pg_stat_errors DROP FUNCTION pg_stat_errors_last();

/* Then we can drop them */
DROP VIEW dba_stat_errors_last;
DROP VIEW pg_stat_errors_last;
DROP FUNCTION pg_stat_errors_last();

/* Now redefine */
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


/* remove functions */

/* First we have to remove them from the extension */
ALTER EXTENSION pg_stat_errors DROP FUNCTION pg_stat_errors_glevel(bigint);
ALTER EXTENSION pg_stat_errors DROP FUNCTION pg_stat_errors_gcode(bigint);
ALTER EXTENSION pg_stat_errors DROP FUNCTION pg_stat_errors_gmessage(bigint);

/* Then we can drop them */
DROP FUNCTION pg_stat_errors_glevel(bigint);
DROP FUNCTION pg_stat_errors_gcode(bigint);
DROP FUNCTION pg_stat_errors_gmessage(bigint);



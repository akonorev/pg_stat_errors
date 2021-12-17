/* pg_stat_errors/pg_stat_errors--1.0.sql */

\echo Use "CREATE EXTENSION pg_stat_errors" to load this file. \quit

-- Register functions.
CREATE FUNCTION pg_stat_errors_reset()
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE FUNCTION pg_stat_errors_glevel(bigint)
RETURNS TEXT
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE FUNCTION pg_stat_errors_gcode(bigint)
RETURNS TEXT
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE FUNCTION pg_stat_errors_gmessage(bigint)
RETURNS TEXT
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


/* pg_stat_errors */
CREATE FUNCTION pg_stat_errors(
    OUT userid              oid,
    OUT dbid                oid,
    OUT elevel              bigint,
    OUT eclass              bigint,
    OUT ecode               bigint,
    ouT errors              bigint,
    OUT last_time           timestamp with time zone
)
RETURNS SETOF record
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT VOLATILE;

CREATE VIEW pg_stat_errors AS
  SELECT * FROM pg_stat_errors();

GRANT SELECT ON pg_stat_errors TO PUBLIC;


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
    pg_stat_errors_glevel(elevel) AS error_level,
    substr(pg_stat_errors_gcode(eclass),1,2) AS error_class,
    pg_stat_errors_gmessage(eclass) AS error_class_message,
    pg_stat_errors_gcode(ecode) AS error_state,
    pg_stat_errors_gmessage(ecode) AS error_state_message,
    errors,
    last_time
FROM pg_stat_errors;

GRANT SELECT ON dba_stat_errors TO PUBLIC;



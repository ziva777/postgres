/* contrib/pageinspect/pageinspect--1.13--1.14.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pageinspect UPDATE TO '1.14'" to load this file. \quit

--
-- heap_page_special
--
CREATE FUNCTION heap_page_special(IN page bytea,
    OUT xid_base int8,
    OUT multi_base int8,
    OUT toast boolean)
AS 'MODULE_PATHNAME', 'heap_page_special'
LANGUAGE C STRICT PARALLEL SAFE;

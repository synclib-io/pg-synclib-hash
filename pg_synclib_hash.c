/**
 * pg_synclib_hash - Postgres extension for computing row hashes
 *
 * Computes SHA256-based row hashes using the synclib_hash library,
 * ensuring cross-platform consistency with iOS/Android/Web clients.
 *
 * Installs as a BEFORE INSERT OR UPDATE trigger that populates
 * a `row_hash` TEXT column on each row.
 *
 * Hash format: SHA256(row_id + "|" + sorted_json(columns))
 *
 * Column selection modes:
 *
 *   Whitelist mode (trigger has arguments):
 *     Only 'id' + named trigger arg columns are included in the hash.
 *     Example: EXECUTE FUNCTION synclib_compute_row_hash('last_modified_ms')
 *     hashes only {id, last_modified_ms}.
 *
 *   Default mode (no trigger arguments):
 *     All columns are included except:
 *       - "row_hash" (the computed column itself — circular dependency)
 *       - Any BYTEA column (binary blobs have no cross-platform canonical form)
 *
 * Type normalization for cross-platform consistency:
 *   - Booleans: true -> 1, false -> 0 (matches SQLite storage)
 *   - Arrays (text[]): converted to JSON array via array_to_json()
 *   - JSONB/JSON: embedded as raw JSON object (keys sorted by synclib)
 *
 * Both server (this trigger) and client (synclibc) build a raw JSON string
 * from column values, then call synclib_build_sorted_json_from_json() to
 * produce canonical sorted JSON. This ensures identical hashes.
 */

#include "postgres.h"
#include "fmgr.h"
#include "commands/trigger.h"
#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/array.h"
#include "utils/json.h"

/* synclib_hash library */
#include "hash.h"

PG_MODULE_MAGIC;

/* ========================================================================== */
/* Helpers                                                                     */
/* ========================================================================== */

/* Check if a type OID represents an array type */
static bool
is_array_type(Oid typoid)
{
    return OidIsValid(get_element_type(typoid));
}

/* Check if a column name is in the trigger argument list (whitelist) */
static int
is_in_whitelist(const char *name, char **args, int nargs)
{
    for (int i = 0; i < nargs; i++)
    {
        if (strcmp(name, args[i]) == 0)
            return 1;
    }
    return 0;
}

/* Ensure buffer capacity, return new buffer (or NULL on failure) */
static char *
ensure_capacity(char *buf, size_t *capacity, size_t pos, size_t needed)
{
    while (pos + needed >= *capacity)
    {
        *capacity *= 2;
        char *new_buf = (char *) repalloc(buf, *capacity);
        if (!new_buf)
            return NULL;
        buf = new_buf;
    }
    return buf;
}

/* Escape a string for JSON (adds surrounding quotes).
 * Returns number of bytes written, or -1 on error. */
static int
json_escape_into(char *buf, size_t buf_size, const char *str)
{
    size_t pos = 0;
    buf[pos++] = '"';

    for (const char *p = str; *p; p++)
    {
        unsigned char ch = (unsigned char)*p;
        if (pos + 8 >= buf_size)
            return -1; /* need more space */

        switch (ch)
        {
            case '"':  buf[pos++] = '\\'; buf[pos++] = '"';  break;
            case '\\': buf[pos++] = '\\'; buf[pos++] = '\\'; break;
            case '\b': buf[pos++] = '\\'; buf[pos++] = 'b';  break;
            case '\f': buf[pos++] = '\\'; buf[pos++] = 'f';  break;
            case '\n': buf[pos++] = '\\'; buf[pos++] = 'n';  break;
            case '\r': buf[pos++] = '\\'; buf[pos++] = 'r';  break;
            case '\t': buf[pos++] = '\\'; buf[pos++] = 't';  break;
            default:
                if (ch < 32)
                    pos += snprintf(buf + pos, 7, "\\u%04x", ch);
                else
                    buf[pos++] = ch;
                break;
        }
    }

    buf[pos++] = '"';
    buf[pos] = '\0';
    return (int)pos;
}

/* ========================================================================== */
/* Trigger function                                                            */
/* ========================================================================== */

PG_FUNCTION_INFO_V1(synclib_compute_row_hash_trigger);

Datum
synclib_compute_row_hash_trigger(PG_FUNCTION_ARGS)
{
    TriggerData    *trigdata;
    HeapTuple       rettuple;
    TupleDesc       tupdesc;
    int             natts;
    int             id_attnum = -1;
    int             row_hash_attnum = -1;
    int             whitelist_nargs;
    char          **whitelist_args;

    /* Validate trigger context */
    if (!CALLED_AS_TRIGGER(fcinfo))
        elog(ERROR, "synclib_compute_row_hash: not called as trigger");

    trigdata = (TriggerData *) fcinfo->context;

    if (!TRIGGER_FIRED_FOR_ROW(trigdata->tg_event))
        elog(ERROR, "synclib_compute_row_hash: must be FOR EACH ROW");

    if (!TRIGGER_FIRED_BEFORE(trigdata->tg_event))
        elog(ERROR, "synclib_compute_row_hash: must be BEFORE trigger");

    /* Read trigger arguments as column whitelist.
     * If arguments are provided, only 'id' + those columns are hashed.
     * If no arguments, all columns are hashed (minus row_hash and bytea). */
    whitelist_nargs = trigdata->tg_trigger->tgnargs;
    whitelist_args = trigdata->tg_trigger->tgargs;

    /* Get the tuple to process */
    if (TRIGGER_FIRED_BY_INSERT(trigdata->tg_event))
        rettuple = trigdata->tg_trigtuple;
    else if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
        rettuple = trigdata->tg_newtuple;
    else
        return PointerGetDatum(trigdata->tg_trigtuple);

    tupdesc = trigdata->tg_relation->rd_att;
    natts = tupdesc->natts;

    /* Find the 'id' and 'row_hash' columns */
    for (int i = 0; i < natts; i++)
    {
        Form_pg_attribute att = TupleDescAttr(tupdesc, i);
        if (att->attisdropped)
            continue;

        const char *attname = NameStr(att->attname);
        if (strcmp(attname, "id") == 0)
            id_attnum = i;
        else if (strcmp(attname, "row_hash") == 0)
            row_hash_attnum = i;
    }

    if (id_attnum < 0)
    {
        elog(WARNING, "synclib_compute_row_hash: no 'id' column, skipping");
        return PointerGetDatum(rettuple);
    }
    if (row_hash_attnum < 0)
    {
        elog(WARNING, "synclib_compute_row_hash: no 'row_hash' column, skipping");
        return PointerGetDatum(rettuple);
    }

    /* Get row ID as text */
    bool    id_isnull;
    Datum   id_datum = heap_getattr(rettuple, id_attnum + 1, tupdesc, &id_isnull);
    if (id_isnull)
        return PointerGetDatum(rettuple);

    Oid     id_typoid = TupleDescAttr(tupdesc, id_attnum)->atttypid;
    Oid     id_typoutput;
    bool    id_typIsVarlena;
    getTypeOutputInfo(id_typoid, &id_typoutput, &id_typIsVarlena);
    char   *row_id = OidOutputFunctionCall(id_typoutput, id_datum);

    /*
     * Build a raw JSON object string from column values, then let
     * synclib_build_sorted_json_from_json() sort keys canonically.
     * This is the same approach used by synclibc (client side).
     */
    size_t json_capacity = 4096;
    char  *json = (char *) palloc(json_capacity);
    size_t pos = 0;
    json[pos++] = '{';

    int first_field = 1;

    for (int i = 0; i < natts; i++)
    {
        Form_pg_attribute att = TupleDescAttr(tupdesc, i);
        if (att->attisdropped)
            continue;

        const char *attname = NameStr(att->attname);
        Oid         typoid = att->atttypid;

        /* Always skip row_hash (circular dependency) */
        if (strcmp(attname, "row_hash") == 0)
            continue;

        if (whitelist_nargs > 0)
        {
            /* Whitelist mode: only include 'id' and columns named in trigger args */
            if (strcmp(attname, "id") != 0 && !is_in_whitelist(attname, whitelist_args, whitelist_nargs))
                continue;
        }
        else
        {
            /* Default mode: only skip bytea (binary blobs) */
            if (typoid == BYTEAOID)
                continue;
        }

        /* Get column value */
        bool    isnull;
        Datum   val = heap_getattr(rettuple, i + 1, tupdesc, &isnull);

        /* Add comma separator */
        if (!first_field)
            json[pos++] = ',';
        first_field = 0;

        /* Add key (quoted) */
        json = ensure_capacity(json, &json_capacity, pos, strlen(attname) + 256);
        if (!json)
            return PointerGetDatum(rettuple);

        /* Simple key escaping (column names are identifiers, no special chars) */
        pos += snprintf(json + pos, json_capacity - pos, "\"%s\":", attname);

        if (isnull)
        {
            memcpy(json + pos, "null", 4);
            pos += 4;
        }
        else if (typoid == BOOLOID)
        {
            /* Normalize boolean to integer for SQLite compatibility */
            if (DatumGetBool(val))
            { json[pos++] = '1'; }
            else
            { json[pos++] = '0'; }
        }
        else if (typoid == INT2OID)
        {
            pos += snprintf(json + pos, json_capacity - pos, "%d",
                            (int) DatumGetInt16(val));
        }
        else if (typoid == INT4OID)
        {
            pos += snprintf(json + pos, json_capacity - pos, "%d",
                            DatumGetInt32(val));
        }
        else if (typoid == INT8OID)
        {
            pos += snprintf(json + pos, json_capacity - pos, "%lld",
                            (long long) DatumGetInt64(val));
        }
        else if (typoid == FLOAT4OID)
        {
            pos += snprintf(json + pos, json_capacity - pos, "%g",
                            (double) DatumGetFloat4(val));
        }
        else if (typoid == FLOAT8OID)
        {
            pos += snprintf(json + pos, json_capacity - pos, "%g",
                            DatumGetFloat8(val));
        }
        else if (typoid == JSONBOID || typoid == JSONOID)
        {
            /* JSON/JSONB: embed as raw JSON (not quoted as a string).
             * The output function gives us JSON text which we embed directly.
             * synclib_build_sorted_json_from_json will sort keys recursively. */
            Oid     typoutput;
            bool    typIsVarlena;
            getTypeOutputInfo(typoid, &typoutput, &typIsVarlena);
            char   *json_text = OidOutputFunctionCall(typoutput, val);
            size_t  jlen = strlen(json_text);

            json = ensure_capacity(json, &json_capacity, pos, jlen + 10);
            if (!json)
                return PointerGetDatum(rettuple);

            memcpy(json + pos, json_text, jlen);
            pos += jlen;
        }
        else if (is_array_type(typoid))
        {
            /* Convert array to JSON array for cross-platform consistency.
             * Postgres text[] "{a,b}" -> JSON ["a","b"] */
            Datum   json_datum = DirectFunctionCall1(array_to_json, val);
            char   *arr_text = TextDatumGetCString(json_datum);
            size_t  alen = strlen(arr_text);

            json = ensure_capacity(json, &json_capacity, pos, alen + 10);
            if (!json)
                return PointerGetDatum(rettuple);

            memcpy(json + pos, arr_text, alen);
            pos += alen;
        }
        else if (typoid == TEXTOID || typoid == VARCHAROID || typoid == BPCHAROID)
        {
            /* Text types: escape for JSON */
            char   *text_val = TextDatumGetCString(val);
            size_t  tlen = strlen(text_val);

            /* Allocate temp buffer for escaped string */
            size_t  esc_capacity = tlen * 6 + 3;
            json = ensure_capacity(json, &json_capacity, pos, esc_capacity);
            if (!json)
                return PointerGetDatum(rettuple);

            int wrote = json_escape_into(json + pos, json_capacity - pos, text_val);
            if (wrote < 0)
            {
                /* Need more space, try again */
                json = ensure_capacity(json, &json_capacity, pos, esc_capacity * 2);
                if (!json)
                    return PointerGetDatum(rettuple);
                wrote = json_escape_into(json + pos, json_capacity - pos, text_val);
            }
            if (wrote > 0)
                pos += wrote;
            else
            {
                memcpy(json + pos, "null", 4);
                pos += 4;
            }
        }
        else
        {
            /* All other types: use PG output function -> text, then escape */
            Oid     typoutput;
            bool    typIsVarlena;
            getTypeOutputInfo(typoid, &typoutput, &typIsVarlena);
            char   *text_val = OidOutputFunctionCall(typoutput, val);
            size_t  tlen = strlen(text_val);

            /* Check if it's a numeric string (for consistency with client) */
            int is_numeric = 1;
            for (size_t k = 0; k < tlen; k++)
            {
                char c = text_val[k];
                if (!((c >= '0' && c <= '9') || c == '-' || c == '.'))
                {
                    is_numeric = 0;
                    break;
                }
            }

            if (is_numeric && tlen > 0)
            {
                json = ensure_capacity(json, &json_capacity, pos, tlen + 10);
                if (!json)
                    return PointerGetDatum(rettuple);
                memcpy(json + pos, text_val, tlen);
                pos += tlen;
            }
            else
            {
                size_t esc_capacity = tlen * 6 + 3;
                json = ensure_capacity(json, &json_capacity, pos, esc_capacity);
                if (!json)
                    return PointerGetDatum(rettuple);

                int wrote = json_escape_into(json + pos, json_capacity - pos, text_val);
                if (wrote < 0)
                {
                    json = ensure_capacity(json, &json_capacity, pos, esc_capacity * 2);
                    if (!json)
                        return PointerGetDatum(rettuple);
                    wrote = json_escape_into(json + pos, json_capacity - pos, text_val);
                }
                if (wrote > 0)
                    pos += wrote;
                else
                {
                    memcpy(json + pos, "null", 4);
                    pos += 4;
                }
            }
        }

        /* Ensure capacity for next iteration */
        json = ensure_capacity(json, &json_capacity, pos, 256);
        if (!json)
            return PointerGetDatum(rettuple);
    }

    json[pos++] = '}';
    json[pos] = '\0';

    /* Use synclib to sort keys and compute hash — single source of truth */
    const char *skip_keys[] = {"row_hash"};
    char *sorted_json = synclib_build_sorted_json_from_json(json, skip_keys, 1);
    char *hash_hex = NULL;

    if (sorted_json)
    {
        hash_hex = synclib_row_hash(row_id, sorted_json);
        free(sorted_json);  /* synclib allocates with malloc */
    }

    /* Set row_hash on the tuple */
    if (hash_hex)
    {
        Datum  *values = (Datum *) palloc(natts * sizeof(Datum));
        bool   *nulls = (bool *) palloc(natts * sizeof(bool));
        bool   *replaces = (bool *) palloc0(natts * sizeof(bool));

        heap_deform_tuple(rettuple, tupdesc, values, nulls);

        replaces[row_hash_attnum] = true;
        values[row_hash_attnum] = CStringGetTextDatum(hash_hex);
        nulls[row_hash_attnum] = false;

        rettuple = heap_modify_tuple(rettuple, tupdesc, values, nulls, replaces);

        free(hash_hex);  /* synclib allocates with malloc */

        pfree(values);
        pfree(nulls);
        pfree(replaces);
    }

    pfree(json);

    return PointerGetDatum(rettuple);
}

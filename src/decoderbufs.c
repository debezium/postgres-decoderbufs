/*
 * decoderbufs - PostgreSQL output plug-in for logical replication to Protocol
 * Buffers
 *
 * Copyright (c) 2014 Xavier Stevens
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#if defined(__linux__)
#include <endian.h>
#elif defined(__APPLE__)
#include <machine/endian.h>
#include <libkern/OSByteOrder.h>
#define htobe64(x) OSSwapHostToBigInt64(x)
#endif
#include <inttypes.h>

#include "postgres.h"
#include "funcapi.h"
#include "catalog/pg_class.h"
#include "catalog/pg_type.h"
#include "catalog/namespace.h"
#include "executor/spi.h"
#include "replication/output_plugin.h"
#include "replication/logical.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/geo_decls.h"
#include "utils/json.h"
#include "utils/memutils.h"
#include "utils/numeric.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/syscache.h"
#include "utils/typcache.h"
#include "utils/uuid.h"
#include "utils/timestamp.h"
#include "utils/date.h"
#include "utils/cash.h"
#include "proto/pg_logicaldec.pb-c.h"

#ifndef HAVE_INT64_TIMESTAMP
#error Expecting timestamps to be represented as integers, not as floating-point.
#endif

#ifdef USE_POSTGIS
/* POSTGIS version define so it doesn't redef macros */
#define POSTGIS_PGSQL_VERSION 94
#include "liblwgeom.h"
#endif

PG_MODULE_MAGIC;

/* define a time macro to convert TimestampTz into something more sane,
 * which in this case is microseconds since epoch. This is because PG stores internally the timestamps relative to
 * 2000-01-01T00:00:00Z and not the Unix epoch.
 */
#define TIMESTAMPTZ_TO_USEC_SINCE_EPOCH(t)    t + (POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * USECS_PER_DAY;
#define DATE_TO_DAYS_SINCE_EPOCH(t)    t + (POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE);

typedef struct {
  MemoryContext context;
  bool debug_mode;
} DecoderData;

/* GLOBALs for PostGIS dynamic OIDs */
Oid geometry_oid = InvalidOid;
Oid geography_oid = InvalidOid;

/* these must be available to pg_dlsym() */
extern void _PG_init(void);
extern void _PG_output_plugin_init(OutputPluginCallbacks *cb);

static void pg_decode_startup(LogicalDecodingContext *ctx,
                              OutputPluginOptions *opt, bool is_init);
static void pg_decode_shutdown(LogicalDecodingContext *ctx);
static void pg_decode_begin_txn(LogicalDecodingContext *ctx,
                                ReorderBufferTXN *txn);
static void pg_decode_commit_txn(LogicalDecodingContext *ctx,
                                 ReorderBufferTXN *txn, XLogRecPtr commit_lsn);
static void pg_decode_change(LogicalDecodingContext *ctx, ReorderBufferTXN *txn,
                             Relation rel, ReorderBufferChange *change);

void _PG_init(void) {}

/* specify output plugin callbacks */
void _PG_output_plugin_init(OutputPluginCallbacks *cb) {
  AssertVariableIsOfType(&_PG_output_plugin_init, LogicalOutputPluginInit);
  cb->startup_cb = pg_decode_startup;
  cb->begin_cb = pg_decode_begin_txn;
  cb->change_cb = pg_decode_change;
  cb->commit_cb = pg_decode_commit_txn;
  cb->shutdown_cb = pg_decode_shutdown;
}

/* initialize this plugin */
static void pg_decode_startup(LogicalDecodingContext *ctx,
                              OutputPluginOptions *opt, bool is_init) {
  ListCell *option;
  DecoderData *data;

  elog(DEBUG1, "Entering startup callback");

  data = palloc(sizeof(DecoderData));
#if PG_VERSION_NUM >= 90600
  data->context = AllocSetContextCreate(
      ctx->context, "decoderbufs context", ALLOCSET_DEFAULT_SIZES);
#else
  data->context = AllocSetContextCreate(
      ctx->context, "decoderbufs context", ALLOCSET_DEFAULT_MINSIZE,
      ALLOCSET_DEFAULT_INITSIZE, ALLOCSET_DEFAULT_MAXSIZE);
#endif
  data->debug_mode = false;
  opt->output_type = OUTPUT_PLUGIN_BINARY_OUTPUT;

  foreach(option, ctx->output_plugin_options) {
    DefElem *elem = lfirst(option);
    Assert(elem->arg == NULL || IsA(elem->arg, String));

    if (strcmp(elem->defname, "debug-mode") == 0) {
      if (elem->arg == NULL) {
        data->debug_mode = false;
      } else if (!parse_bool(strVal(elem->arg), &data->debug_mode)) {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("could not parse value \"%s\" for parameter \"%s\"",
                        strVal(elem->arg), elem->defname)));
      }

      if (data->debug_mode) {
        elog(NOTICE, "Decoderbufs DEBUG MODE is ON.");
        opt->output_type = OUTPUT_PLUGIN_TEXTUAL_OUTPUT;
      } else {
        elog(NOTICE, "Decoderbufs DEBUG MODE is OFF.");
      }
    } else {
      ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                      errmsg("option \"%s\" = \"%s\" is unknown", elem->defname,
                             elem->arg ? strVal(elem->arg) : "(null)")));
    }
  }

  ctx->output_plugin_private = data;

  elog(INFO, "Exiting startup callback");
}

/* cleanup this plugin's resources */
static void pg_decode_shutdown(LogicalDecodingContext *ctx) {
  DecoderData *data;

  elog(DEBUG1, "Entering decode_shutdown callback");

  data = ctx->output_plugin_private;

  /* cleanup our own resources via memory context reset */
  MemoryContextDelete(data->context);
}

/* BEGIN callback */
static void pg_decode_begin_txn(LogicalDecodingContext *ctx,
                                ReorderBufferTXN *txn) {
#ifdef USE_POSTGIS
  // set PostGIS geometry type id (these are dynamic)
  // TODO: Figure out how to make sure we get the typid's from postgis extension namespace
  if (geometry_oid == InvalidOid) {
    geometry_oid = TypenameGetTypid("geometry");
    if (geometry_oid != InvalidOid) {
      elog(DEBUG1, "PostGIS geometry type detected: %u", geometry_oid);
    }
  }
  if (geography_oid == InvalidOid) {
    geography_oid = TypenameGetTypid("geography");
    if (geography_oid != InvalidOid) {
      elog(DEBUG1, "PostGIS geometry type detected: %u", geography_oid);
    }
  }
#endif
}

/* COMMIT callback */
static void pg_decode_commit_txn(LogicalDecodingContext *ctx,
                                 ReorderBufferTXN *txn, XLogRecPtr commit_lsn) {
}

/* print tuple datums (only used for debug-mode) */
static void print_tuple_datums(StringInfo out, Decoderbufs__DatumMessage **tup,
                               size_t n) {
  if (tup) {
    for (int i = 0; i < n; i++) {
      Decoderbufs__DatumMessage *dmsg = tup[i];

      if (dmsg->column_name)
        appendStringInfo(out, "column_name[%s]", dmsg->column_name);

      if (dmsg->has_column_type)
        appendStringInfo(out, ", column_type[%" PRId64 "]", dmsg->column_type);

      switch (dmsg->datum_case) {
        case DECODERBUFS__DATUM_MESSAGE__DATUM_DATUM_INT32:
          appendStringInfo(out, ", datum[%d]", dmsg->datum_int32);
          break;
        case DECODERBUFS__DATUM_MESSAGE__DATUM_DATUM_INT64:
          appendStringInfo(out, ", datum[%" PRId64 "]", dmsg->datum_int64);
          break;
        case DECODERBUFS__DATUM_MESSAGE__DATUM_DATUM_FLOAT:
          appendStringInfo(out, ", datum[%f]", dmsg->datum_float);
          break;
        case DECODERBUFS__DATUM_MESSAGE__DATUM_DATUM_DOUBLE:
          appendStringInfo(out, ", datum[%f]", dmsg->datum_double);
          break;
        case DECODERBUFS__DATUM_MESSAGE__DATUM_DATUM_BOOL:
          appendStringInfo(out, ", datum[%d]", dmsg->datum_bool);
          break;
        case DECODERBUFS__DATUM_MESSAGE__DATUM_DATUM_STRING:
          appendStringInfo(out, ", datum[%s]", dmsg->datum_string);
          break;
        case DECODERBUFS__DATUM_MESSAGE__DATUM_DATUM_BYTES:
          break;
        case DECODERBUFS__DATUM_MESSAGE__DATUM_DATUM_POINT:
          appendStringInfo(out, ", datum[POINT(%f, %f)]",
                           dmsg->datum_point->x, dmsg->datum_point->y);
          break;
        case DECODERBUFS__DATUM_MESSAGE__DATUM__NOT_SET:
          // intentional fall-through
        default:
          appendStringInfo(out, ", datum[!NOT SET!]");
          break;
      }
      appendStringInfo(out, "\n");
    }
  }
}

/* print a row message (only used for debug-mode) */
static void print_row_msg(StringInfo out, Decoderbufs__RowMessage *rmsg) {
  if (!rmsg)
    return;

  if (rmsg->has_transaction_id)
    appendStringInfo(out, "txid[%d]", rmsg->transaction_id);

  if (rmsg->has_commit_time)
    appendStringInfo(out, ", commit_time[%" PRId64 "]", rmsg->commit_time);

  if (rmsg->table)
    appendStringInfo(out, ", table[%s]", rmsg->table);

  if (rmsg->has_op)
    appendStringInfo(out, ", op[%d]", rmsg->op);

  if (rmsg->old_tuple) {
    appendStringInfo(out, "\nOLD TUPLE: \n");
    print_tuple_datums(out, rmsg->old_tuple, rmsg->n_old_tuple);
    appendStringInfo(out, "\n");
  }

  if (rmsg->new_tuple) {
    appendStringInfo(out, "\nNEW TUPLE: \n");
    print_tuple_datums(out, rmsg->new_tuple, rmsg->n_new_tuple);
    appendStringInfo(out, "\n");
  }

}

/* this doesn't seem to be available in the public api (unfortunate) */
static double numeric_to_double_no_overflow(Numeric num) {
  char *tmp;
  double val;
  char *endptr;

  tmp = DatumGetCString(DirectFunctionCall1(numeric_out, NumericGetDatum(num)));

  /* unlike float8in, we ignore ERANGE from strtod */
  val = strtod(tmp, &endptr);
  if (*endptr != '\0') {
    /* shouldn't happen ... */
    ereport(ERROR,
            (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
             errmsg("invalid input syntax for type double precision: \"%s\"",
                    tmp)));
  }

  return val;
}

static bool geography_point_as_decoderbufs_point(Datum datum,
                                                 Decoderbufs__Point *p) {
#ifdef USE_POSTGIS
  GSERIALIZED *geom;
  LWGEOM *lwgeom;
  LWPOINT *point = NULL;
  POINT2D p2d;

  geom = (GSERIALIZED *)PG_DETOAST_DATUM(datum);
  if (gserialized_get_type(geom) != POINTTYPE) {
    return false;
  }

  lwgeom = lwgeom_from_gserialized(geom);
  point = lwgeom_as_lwpoint(lwgeom);
  if (lwgeom_is_empty(lwgeom)) {
    return false;
  }

  getPoint2d_p(point->point, 0, &p2d);

  if (p != NULL) {
    Decoderbufs__Point dp = DECODERBUFS__POINT__INIT;
    dp.x = p2d.x;
    dp.y = p2d.y;
    memcpy(p, &dp, sizeof(dp));
    elog(DEBUG1, "Translating geography to point: (x,y) = (%f,%f)", p->x, p->y);
  }

  return true;
#else
  elog(DEBUG1, "PostGIS support is off, recompile decoderbufs with USE_POSTGIS option!");
  return false;
#endif
}

/* set a datum value based on its OID specified by typid */
static void set_datum_value(Decoderbufs__DatumMessage *datum_msg, Oid typid,
                            Oid typoutput, Datum datum) {
  Numeric num;
  bytea *valptr = NULL;
  const char *output = NULL;
  Point *p = NULL;
  Timestamp ts;
  double duration;
  TimeTzADT *timetz = NULL;
  Interval   *interval = NULL;
  Decoderbufs__Point dp = DECODERBUFS__POINT__INIT;
     
  int size = 0;
  switch (typid) {
    case BOOLOID:
      datum_msg->datum_bool = DatumGetBool(datum);
      datum_msg->datum_case = DECODERBUFS__DATUM_MESSAGE__DATUM_DATUM_BOOL;
      break;
    case INT2OID:
      datum_msg->datum_int32 = DatumGetInt16(datum);
      datum_msg->datum_case = DECODERBUFS__DATUM_MESSAGE__DATUM_DATUM_INT32;
      break;
    case INT4OID:
      datum_msg->datum_int32 = DatumGetInt32(datum);
      datum_msg->datum_case = DECODERBUFS__DATUM_MESSAGE__DATUM_DATUM_INT32;
      break;
    case INT8OID:
    case OIDOID:
      datum_msg->datum_int64 = DatumGetInt64(datum);
      datum_msg->datum_case = DECODERBUFS__DATUM_MESSAGE__DATUM_DATUM_INT64;
      break;
    case FLOAT4OID:
      datum_msg->datum_float = DatumGetFloat4(datum);
      datum_msg->datum_case = DECODERBUFS__DATUM_MESSAGE__DATUM_DATUM_FLOAT;
      break;
    case FLOAT8OID:
      datum_msg->datum_double = DatumGetFloat8(datum);
      datum_msg->datum_case = DECODERBUFS__DATUM_MESSAGE__DATUM_DATUM_DOUBLE;
      break;
    case CASHOID: 
      datum_msg->datum_int64 = DatumGetCash(datum);
      datum_msg->datum_case = DECODERBUFS__DATUM_MESSAGE__DATUM_DATUM_INT64;
      break;
    case NUMERICOID:
    case CHAROID:
    case VARCHAROID:
    case BPCHAROID:
    case TEXTOID:
    case JSONOID:
    case JSONBOID: 
    case XMLOID:
    case BITOID:
    case VARBITOID:
    case UUIDOID:
      output = OidOutputFunctionCall(typoutput, datum);
      datum_msg->datum_string = pnstrdup(output, strlen(output));
      datum_msg->datum_case = DECODERBUFS__DATUM_MESSAGE__DATUM_DATUM_STRING;
      break;
    case TIMESTAMPOID:
      ts = DatumGetTimestamp(datum);
      if (TIMESTAMP_NOT_FINITE(ts)) {
           ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
           errmsg("timestamp \'%s\'out of range", ts ? strVal(ts) : "(null)")));
      } else {
           datum_msg->datum_int64 = TIMESTAMPTZ_TO_USEC_SINCE_EPOCH(ts);
           datum_msg->datum_case = DECODERBUFS__DATUM_MESSAGE__DATUM_DATUM_INT64;
           break;       
      }
    case TIMESTAMPTZOID:
      ts = DatumGetTimestampTz(datum);
      if (TIMESTAMP_NOT_FINITE(ts)) {
           ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
           errmsg("timestamp \'%s\'out of range", ts ? strVal(ts) : "(null)")));
       } else {
           datum_msg->datum_int64 = TIMESTAMPTZ_TO_USEC_SINCE_EPOCH(ts);
           datum_msg->datum_case = DECODERBUFS__DATUM_MESSAGE__DATUM_DATUM_INT64;
           break;       
        }
    case DATEOID: 
      /* simply get the number of days as the stored 32 bit value and convert to EPOCH */
      datum_msg->datum_int32 = DATE_TO_DAYS_SINCE_EPOCH(DatumGetDateADT(datum));
      datum_msg->datum_case = DECODERBUFS__DATUM_MESSAGE__DATUM_DATUM_INT32;
      break;
    case TIMEOID:
      datum_msg->datum_int64 = DatumGetTimeADT(datum);
      datum_msg->datum_case = DECODERBUFS__DATUM_MESSAGE__DATUM_DATUM_INT64;
      break;                                                 
    case TIMETZOID:
      timetz = DatumGetTimeTzADTP(datum);
      /* use GMT-equivalent time */     
      datum_msg->datum_double = (double) (timetz->time + (timetz->zone * 1000000.0));
      datum_msg->datum_case = DECODERBUFS__DATUM_MESSAGE__DATUM_DATUM_DOUBLE;
      break;     
    case INTERVALOID:
      interval = DatumGetIntervalP(datum);
      /* 
        Convert the month part of Interval to days using assumed average month length of 365.25/12.0 days. 
      */  
      duration = interval->time + interval->day * (double) USECS_PER_DAY + interval->month * ((DAYS_PER_YEAR / (double) MONTHS_PER_YEAR) * USECS_PER_DAY);
      datum_msg->datum_double = duration;
      datum_msg->datum_case = DECODERBUFS__DATUM_MESSAGE__DATUM_DATUM_DOUBLE;
      break;
    case BYTEAOID:
      valptr = DatumGetByteaPCopy(datum);
      size = VARSIZE(valptr) - VARHDRSZ;
      datum_msg->datum_bytes.data = palloc(size);
      memcpy(datum_msg->datum_bytes.data, (uint8_t *)VARDATA(valptr), size);
      datum_msg->datum_bytes.len = size;
      datum_msg->datum_case = DECODERBUFS__DATUM_MESSAGE__DATUM_DATUM_BYTES;
      break;
    case POINTOID:
      p = DatumGetPointP(datum);
      dp.x = p->x;
      dp.y = p->y;
      datum_msg->datum_point = palloc(sizeof(Decoderbufs__Point));
      memcpy(datum_msg->datum_point, &dp, sizeof(dp));
      datum_msg->datum_case = DECODERBUFS__DATUM_MESSAGE__DATUM_DATUM_POINT;
      break;
    default:
      // PostGIS uses dynamic OIDs so we need to check the type again here
      if (typid == geometry_oid || typid == geography_oid) {
        elog(DEBUG1, "Converting geography point to datum_point");
        datum_msg->datum_point = palloc(sizeof(Decoderbufs__Point));
        geography_point_as_decoderbufs_point(datum, datum_msg->datum_point);
        datum_msg->datum_case = DECODERBUFS__DATUM_MESSAGE__DATUM_DATUM_POINT;
      } else {
        int len;
        elog(DEBUG1, "Encountered unknown typid: %d, using bytes", typid);
        output = OidOutputFunctionCall(typoutput, datum);
        len = strlen(output);
        size = sizeof(char) * len;
        datum_msg->datum_bytes.data = palloc(size);
        memcpy(datum_msg->datum_bytes.data, (uint8_t *)output, size);
        datum_msg->datum_bytes.len = len;
        datum_msg->datum_case = DECODERBUFS__DATUM_MESSAGE__DATUM_DATUM_BYTES;
      }
      break;
  }
}

/* return the number of valid attributes from the tuple */
static int valid_attributes_count_from(TupleDesc tupdesc) {
  int natt;
  int count = 0;
  for (natt = 0; natt < tupdesc->natts; natt++) {
      Form_pg_attribute attr = TupleDescAttr(tupdesc, natt);
      
      /* skip dropped columns and system columns */
      if (attr->attisdropped  || attr->attnum < 0) {       
        continue;
      }    
      count++;      
  }
  return count;    
}

/* convert a PG tuple to an array of DatumMessage(s) */
static void tuple_to_tuple_msg(Decoderbufs__DatumMessage **tmsg,
                               Relation relation, HeapTuple tuple,
                               TupleDesc tupdesc) {
  int natt;
  int valid_attr_cnt = 0;
  elog(DEBUG1, "processing tuple with %d columns", tupdesc->natts);
  /* build column names and values */
  for (natt = 0; natt < tupdesc->natts; natt++) {
    Form_pg_attribute attr;
    Datum origval;
    bool isnull;
    const char *attrName;
    Oid typoutput;
    bool typisvarlena;
    Decoderbufs__DatumMessage datum_msg = DECODERBUFS__DATUM_MESSAGE__INIT;

    attr = TupleDescAttr(tupdesc, natt);
    
    /* skip dropped columns and system columns */
    if (attr->attisdropped || attr->attnum < 0) {
      elog(DEBUG1, "skipping column %d because %s", natt + 1, attr->attisdropped ? "it's a dropped column" : "it's a system column");
      continue;
    }    

    attrName = quote_identifier(NameStr(attr->attname));     
    elog(DEBUG1, "processing column %d with name %s", natt + 1, attrName);    

    /* set the column name */
    datum_msg.column_name = (char *)attrName;
    
    /* set datum from tuple */
    origval = heap_getattr(tuple, natt + 1, tupdesc, &isnull);

    /* get output function */
    datum_msg.column_type = attr->atttypid;
    datum_msg.has_column_type = true;

    /* query output function */
    getTypeOutputInfo(attr->atttypid, &typoutput, &typisvarlena);
    if (!isnull) {
      if (typisvarlena && VARATT_IS_EXTERNAL_ONDISK(origval)) {
        // TODO: Is there a way we can handle this?
        elog(DEBUG1, "Not handling external on disk varlena at the moment.");
      } else if (!typisvarlena) {
        set_datum_value(&datum_msg, attr->atttypid, typoutput, origval);
      } else {
        Datum val = PointerGetDatum(PG_DETOAST_DATUM(origval));
        set_datum_value(&datum_msg, attr->atttypid, typoutput, val);
      }
    } else {
      elog(DEBUG1, "column %s is null, ignoring value", attrName);  
    }

    tmsg[valid_attr_cnt] = palloc(sizeof(datum_msg));
    memcpy(tmsg[valid_attr_cnt], &datum_msg, sizeof(datum_msg));

    valid_attr_cnt++;
  }
}

/* provide a metadata for new tuple */
static void add_metadata_to_msg(Decoderbufs__TypeInfo **tmsg,
                                Relation relation, HeapTuple tuple,
                                TupleDesc tupdesc) {
  int natt;
  int valid_attr_cnt = 0;
  elog(DEBUG1, "Adding metadata for %d columns", tupdesc->natts);
  /* build column names and values */
  for (natt = 0; natt < tupdesc->natts; natt++) {
    Form_pg_attribute attr;
    char *typ_mod;
    Decoderbufs__TypeInfo typeinfo = DECODERBUFS__TYPE_INFO__INIT;
    bool not_null;

    attr = TupleDescAttr(tupdesc, natt);

    /* skip dropped columns and system columns */
    if (attr->attisdropped || attr->attnum < 0) {
      elog(DEBUG1, "skipping column %d because %s", natt + 1, attr->attisdropped ? "it's a dropped column" : "it's a system column");
      continue;
    }

    not_null = attr->attnotnull;
    typ_mod = TextDatumGetCString(DirectFunctionCall2(format_type, attr->atttypid, attr->atttypmod));
    elog(DEBUG1, "Adding typemodifier '%s' for column %d, optional %s", typ_mod, natt, !not_null ? "true" : "false");

    typeinfo.modifier = typ_mod;
    typeinfo.value_optional = !not_null;
    tmsg[valid_attr_cnt] = palloc(sizeof(typeinfo));
    memcpy(tmsg[valid_attr_cnt], &typeinfo, sizeof(typeinfo));

    valid_attr_cnt++;
  }
}

/* callback for individual changed tuples */
static void pg_decode_change(LogicalDecodingContext *ctx, ReorderBufferTXN *txn,
                             Relation relation, ReorderBufferChange *change) {
  DecoderData *data;
  MemoryContext old;

  Form_pg_class class_form;
  char replident;
  bool is_rel_non_selective;
  const char *selectiveInfo;
  TupleDesc tupdesc;
  Decoderbufs__RowMessage rmsg = DECODERBUFS__ROW_MESSAGE__INIT;

  elog(DEBUG1, "Entering decode_change callback");

  /* Avoid leaking memory by using and resetting our own context */
  data = ctx->output_plugin_private;
  old = MemoryContextSwitchTo(data->context);

  replident = relation->rd_rel->relreplident;

  class_form = RelationGetForm(relation);

  RelationGetIndexList(relation);
  is_rel_non_selective = (replident == REPLICA_IDENTITY_NOTHING ||
                          (replident == REPLICA_IDENTITY_DEFAULT &&
                           !OidIsValid(relation->rd_replidindex)));
  selectiveInfo = is_rel_non_selective ? "non selective" : "selective";

  /* set common fields */
  rmsg.transaction_id = txn->xid;
  rmsg.has_transaction_id = true;
  rmsg.commit_time = TIMESTAMPTZ_TO_USEC_SINCE_EPOCH(txn->commit_time);
  rmsg.has_commit_time = true;
  rmsg.table = pstrdup(quote_qualified_identifier(get_namespace_name(get_rel_namespace(RelationGetRelid(relation))), 
    NameStr(class_form->relname)));
  
 
  /* decode different operation types */
  switch (change->action) {
    case REORDER_BUFFER_CHANGE_INSERT:
      elog(DEBUG1, "decoding INSERT for table %s; relation is %s", rmsg.table, selectiveInfo);
      rmsg.op = DECODERBUFS__OP__INSERT;
      rmsg.has_op = true;
      if (change->data.tp.newtuple != NULL) {
        elog(DEBUG1, "decoding new tuple information");
        tupdesc = RelationGetDescr(relation);

        rmsg.n_new_tuple = valid_attributes_count_from(tupdesc);
        rmsg.new_tuple =
            palloc(sizeof(Decoderbufs__DatumMessage*) * rmsg.n_new_tuple);
        tuple_to_tuple_msg(rmsg.new_tuple, relation,
                           &change->data.tp.newtuple->tuple, tupdesc);

        rmsg.n_new_typeinfo = rmsg.n_new_tuple;
        rmsg.new_typeinfo =
            palloc(sizeof(Decoderbufs__TypeInfo*) * rmsg.n_new_typeinfo);
        add_metadata_to_msg(rmsg.new_typeinfo, relation,
                           &change->data.tp.newtuple->tuple, tupdesc);
      }
      break;
    case REORDER_BUFFER_CHANGE_UPDATE:
      rmsg.op = DECODERBUFS__OP__UPDATE;
      rmsg.has_op = true;
      elog(DEBUG1, "decoding UPDATE for table %s; relation is %s", rmsg.table, selectiveInfo);
      if (!is_rel_non_selective) {
        if (change->data.tp.oldtuple != NULL) {
          elog(DEBUG1, "decoding old tuple information");
          tupdesc = RelationGetDescr(relation);
          rmsg.n_old_tuple =  valid_attributes_count_from(tupdesc);
          rmsg.old_tuple =
              palloc(sizeof(Decoderbufs__DatumMessage*) * rmsg.n_old_tuple);
          tuple_to_tuple_msg(rmsg.old_tuple, relation,
                             &change->data.tp.oldtuple->tuple, tupdesc);
        }
        if (change->data.tp.newtuple != NULL) {
          elog(DEBUG1, "decoding new tuple information");
          tupdesc = RelationGetDescr(relation);

          rmsg.n_new_tuple = valid_attributes_count_from(tupdesc);
          rmsg.new_tuple =
              palloc(sizeof(Decoderbufs__DatumMessage*) * rmsg.n_new_tuple);
          tuple_to_tuple_msg(rmsg.new_tuple, relation,
                             &change->data.tp.newtuple->tuple, tupdesc);

          rmsg.n_new_typeinfo = rmsg.n_new_tuple;
          rmsg.new_typeinfo =
              palloc(sizeof(Decoderbufs__TypeInfo*) * rmsg.n_new_typeinfo);
          add_metadata_to_msg(rmsg.new_typeinfo, relation,
                             &change->data.tp.newtuple->tuple, tupdesc);
        }
      }
      break;
    case REORDER_BUFFER_CHANGE_DELETE:
      rmsg.op = DECODERBUFS__OP__DELETE;
      rmsg.has_op = true;
      elog(DEBUG1, "decoding DELETE for table %s; relation is %s", rmsg.table, selectiveInfo);
      /* if there was no PK, we only know that a delete happened */
      if (!is_rel_non_selective && change->data.tp.oldtuple != NULL) {
        elog(DEBUG1, "decoding old tuple information");
        tupdesc = RelationGetDescr(relation);
        rmsg.n_old_tuple = valid_attributes_count_from(tupdesc);
        rmsg.old_tuple =
            palloc(sizeof(Decoderbufs__DatumMessage*) *  rmsg.n_old_tuple);
        tuple_to_tuple_msg(rmsg.old_tuple, relation,
                           &change->data.tp.oldtuple->tuple, tupdesc);
      } else {
        elog(DEBUG1, "no information to decode from DELETE because either no PK is present or REPLICA IDENTITY NOTHING or invalid ");
      }
      break;
    default:
      elog(WARNING, "unknown change action");       
      Assert(0);
      break;
  }

  /* write msg */
  OutputPluginPrepareWrite(ctx, true);
  if (data->debug_mode) {
    //protobuf_c_text_to_string(ctx->out, (ProtobufCMessage*)&rmsg);
    print_row_msg(ctx->out, &rmsg);
  } else {
    size_t psize = decoderbufs__row_message__get_packed_size(&rmsg);
    void *packed = palloc(psize);
    size_t ssize = decoderbufs__row_message__pack(&rmsg, packed);
    appendBinaryStringInfo(ctx->out, packed, ssize);
  }
  OutputPluginWrite(ctx, true);

  /* Cleanup, freeing memory */
  MemoryContextSwitchTo(old);
  MemoryContextReset(data->context);
}

/*
  +----------------------------------------------------------------------+
  | PHP Version 7                                                        |
  +----------------------------------------------------------------------+
  | Copyright (c) 1997-2017 The PHP Group                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Author: Wez Furlong <wez@php.net>                                    |
  |         Frank M. Kromann <frank@kromann.info>                        |
  +----------------------------------------------------------------------+
*/

/* $Id$ */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/php_string.h"
#include "ext/standard/info.h"
#include "pdo/php_pdo.h"
#include "pdo/php_pdo_driver.h"
#include "php_pdo_dblib.h"
#include "php_pdo_dblib_int.h"
#include "zend_exceptions.h"


#define DBLIB_MEMCPY_WALK(dst, src, len) if (memcpy(dst, src, len)) dst += len
static int pdo_dblib_rpc_execute(pdo_stmt_t *stmt);


/* {{{ pdo_dblib_get_field_name
 *
 * Return the data type name for a given TDS number
 *
 */
static char *pdo_dblib_get_field_name(int type)
{
	/*
	 * I don't return dbprtype(type) because it does not fully describe the type
	 * (example: varchar is reported as char by dbprtype)
	 *
	 * FIX ME: Cache datatypes from server systypes table in pdo_dblib_handle_factory()
	 * 		   to make this future proof.
	 */

	switch (type) {
		case 31: return "nvarchar";
		case 34: return "image";
		case 35: return "text";
		case 36: return "uniqueidentifier";
		case 37: return "varbinary"; /* & timestamp - Sybase AS12 */
		case 38: return "bigint"; /* & bigintn - Sybase AS12 */
		case 39: return "varchar"; /* & sysname & nvarchar - Sybase AS12 */
		case 40: return "date";
		case 41: return "time";
		case 42: return "datetime2";
		case 43: return "datetimeoffset";
		case 45: return "binary"; /* Sybase AS12 */
		case 47: return "char"; /* & nchar & uniqueidentifierstr Sybase AS12 */
		case 48: return "tinyint";
		case 50: return "bit"; /* Sybase AS12 */
		case 52: return "smallint";
		case 55: return "decimal"; /* Sybase AS12 */
		case 56: return "int";
		case 58: return "smalldatetime";
		case 59: return "real";
		case 60: return "money";
		case 61: return "datetime";
		case 62: return "float";
		case 63: return "numeric"; /* or uint, ubigint, usmallint Sybase AS12 */
		case 98: return "sql_variant";
		case 99: return "ntext";
		case 104: return "bit";
		case 106: return "decimal"; /* decimal n on sybase */
		case 108: return "numeric"; /* numeric n on sybase */
		case 122: return "smallmoney";
		case 127: return "bigint";
		case 165: return "varbinary";
		case 167: return "varchar";
		case 173: return "binary";
		case 175: return "char";
		case 189: return "timestamp";
		case 231: return "nvarchar";
		case 239: return "nchar";
		case 240: return "geometry";
		case 241: return "xml";
		default: return "unknown";
	}
}
/* }}} */

static int pdo_dblib_stmt_cursor_closer(pdo_stmt_t *stmt)
{
	pdo_dblib_stmt *S = (pdo_dblib_stmt*)stmt->driver_data;
	pdo_dblib_db_handle *H = S->H;

	/* Cancel any pending results */
	dbcancel(H->link);

	pdo_dblib_err_dtor(&H->err);

	return 1;
}

static int pdo_dblib_stmt_dtor(pdo_stmt_t *stmt)
{
	pdo_dblib_stmt *S = (pdo_dblib_stmt*)stmt->driver_data;
	pdo_dblib_rpc_stmt *rpc = S->rpc;

	if (rpc) {
		if (rpc->params) {
			efree(rpc->params);
		}
		efree(rpc);
	}

	pdo_dblib_err_dtor(&S->err);

	efree(S);

	return 1;
}

static int pdo_dblib_stmt_next_rowset_no_cancel(pdo_stmt_t *stmt)
{
	pdo_dblib_stmt *S = (pdo_dblib_stmt*)stmt->driver_data;
	pdo_dblib_db_handle *H = S->H;
	RETCODE ret;
	int num_fields;

	do {
		ret = dbresults(H->link);
		num_fields = dbnumcols(H->link);
	} while (H->skip_empty_rowsets && num_fields <= 0 && ret == SUCCEED);


	if (FAIL == ret) {
		pdo_raise_impl_error(stmt->dbh, stmt, "HY000", "PDO_DBLIB: dbresults() returned FAIL");
		return 0;
	}

	if (NO_MORE_RESULTS == ret) {
		return 0;
	}

	if (H->skip_empty_rowsets && num_fields <= 0) {
		return 0;
	}

	stmt->row_count = DBCOUNT(H->link);
	stmt->column_count = num_fields;

	return 1;
}

static int pdo_dblib_stmt_next_rowset(pdo_stmt_t *stmt)
{
	pdo_dblib_stmt *S = (pdo_dblib_stmt*)stmt->driver_data;
	pdo_dblib_db_handle *H = S->H;
	RETCODE ret = SUCCESS;

	/* Ideally use dbcanquery here, but there is a bug in FreeTDS's implementation of dbcanquery
	 * It has been resolved but is currently only available in nightly builds
	 */
	while (NO_MORE_ROWS != ret) {
		ret = dbnextrow(H->link);

		if (FAIL == ret) {
			pdo_raise_impl_error(stmt->dbh, stmt, "HY000", "PDO_DBLIB: dbnextrow() returned FAIL");
			return 0;
		}
	}

	return pdo_dblib_stmt_next_rowset_no_cancel(stmt);
}

static int pdo_dblib_stmt_execute(pdo_stmt_t *stmt)
{
	pdo_dblib_stmt *S = (pdo_dblib_stmt*)stmt->driver_data;
	pdo_dblib_db_handle *H = S->H;

	dbsetuserdata(H->link, (BYTE*) &S->err);

	pdo_dblib_stmt_cursor_closer(stmt);

	if (S->rpc) {
		if (!pdo_dblib_rpc_execute(stmt)) {
			return 0;
		}

		if (S->rpc->skip_results) {
			while (SUCCEED == dbresults(H->link)) {
				while(NO_MORE_ROWS != dbnextrow(H->link)) {};
			}
			return 1;
		}
	} else {
		if (FAIL == dbcmd(H->link, stmt->active_query_string)) {
			return 0;
		}

		if (FAIL == dbsqlexec(H->link)) {
			return 0;
		}
	}

	pdo_dblib_stmt_next_rowset_no_cancel(stmt);

	stmt->row_count = DBCOUNT(H->link);
	stmt->column_count = dbnumcols(H->link);

	return 1;
}

static int pdo_dblib_stmt_fetch(pdo_stmt_t *stmt,
	enum pdo_fetch_orientation ori, zend_long offset)
{

	RETCODE ret;

	pdo_dblib_stmt *S = (pdo_dblib_stmt*)stmt->driver_data;
	pdo_dblib_db_handle *H = S->H;

	ret = dbnextrow(H->link);

	if (FAIL == ret) {
		pdo_raise_impl_error(stmt->dbh, stmt, "HY000", "PDO_DBLIB: dbnextrow() returned FAIL");
		return 0;
	}

	if(NO_MORE_ROWS == ret) {
		return 0;
	}

	return 1;
}

static int pdo_dblib_stmt_describe(pdo_stmt_t *stmt, int colno)
{
	pdo_dblib_stmt *S = (pdo_dblib_stmt*)stmt->driver_data;
	pdo_dblib_db_handle *H = S->H;
	struct pdo_column_data *col;
	char *fname;

	if(colno >= stmt->column_count || colno < 0)  {
		return FAILURE;
	}

	if (colno == 0) {
		S->computed_column_name_count = 0;
	}

	col = &stmt->columns[colno];
	fname = (char*)dbcolname(H->link, colno+1);

	if (fname && *fname) {
		col->name =  zend_string_init(fname, strlen(fname), 0);
	} else {
		if (S->computed_column_name_count > 0) {
			char buf[16];
			int len;

			len = snprintf(buf, sizeof(buf), "computed%d", S->computed_column_name_count);
			col->name = zend_string_init(buf, len, 0);
		} else {
			col->name = zend_string_init("computed", strlen("computed"), 0);
		}

		S->computed_column_name_count++;
	}

	col->maxlen = dbcollen(H->link, colno+1);
	col->param_type = PDO_PARAM_ZVAL;

	return 1;
}

static int pdo_dblib_stmt_should_stringify_col(pdo_stmt_t *stmt, int coltype)
{
	pdo_dblib_stmt *S = (pdo_dblib_stmt*)stmt->driver_data;
	pdo_dblib_db_handle *H = S->H;

	switch (coltype) {
		case SQLDECIMAL:
		case SQLNUMERIC:
		case SQLMONEY:
		case SQLMONEY4:
		case SQLMONEYN:
		case SQLFLT4:
		case SQLFLT8:
		case SQLINT4:
		case SQLINT2:
		case SQLINT1:
		case SQLBIT:
			if (stmt->dbh->stringify) {
				return 1;
			}
			break;

		case SQLINT8:
			if (stmt->dbh->stringify) {
				return 1;
			}

			/* force stringify if DBBIGINT won't fit in zend_long */
			/* this should only be an issue for 32-bit machines */
			if (sizeof(zend_long) < sizeof(DBBIGINT)) {
				return 1;
			}
			break;

#ifdef SQLMSDATETIME2
		case SQLMSDATETIME2:
#endif
		case SQLDATETIME:
		case SQLDATETIM4:
			if (H->datetime_convert) {
				return 1;
			}
			break;
	}

	return 0;
}

static void pdo_dblib_stmt_stringify_col(int coltype, LPBYTE data, DBINT data_len, zval **ptr)
{
	DBCHAR *tmp_data;
	DBINT tmp_data_len;
	zval *zv;

	/* FIXME: We allocate more than we need here */
	tmp_data_len = 32 + (2 * (data_len));

	switch (coltype) {
		case SQLDATETIME:
		case SQLDATETIM4: {
			if (tmp_data_len < DATETIME_MAX_LEN) {
				tmp_data_len = DATETIME_MAX_LEN;
			}
			break;
		}
	}

	tmp_data = emalloc(tmp_data_len);
	data_len = dbconvert(NULL, coltype, data, data_len, SQLCHAR, (LPBYTE) tmp_data, tmp_data_len);

	zv = emalloc(sizeof(zval));
	if (data_len > 0) {
		/* to prevent overflows, tmp_data_len is provided as a dest len for dbconvert()
		 * this code previously passed a dest len of -1
		 * the FreeTDS impl of dbconvert() does an rtrim with that value, so replicate that behavior
		 */
		while (data_len > 0 && tmp_data[data_len - 1] == ' ') {
			data_len--;
		}

		ZVAL_STRINGL(zv, tmp_data, data_len);
	} else {
		ZVAL_EMPTY_STRING(zv);
	}

	efree(tmp_data);

	*ptr = zv;
}

static int pdo_dblib_stmt_get_col_or_ret(pdo_stmt_t *stmt, int colno, char **ptr,
	 zend_ulong *len, int is_ret)
{
	pdo_dblib_stmt *S = (pdo_dblib_stmt*)stmt->driver_data;
	pdo_dblib_db_handle *H = S->H;

	int coltype;
	LPBYTE data;
	DBCHAR *tmp_data;
	DBINT data_len, tmp_data_len;
	zval *zv = NULL;

	if (is_ret) {
		coltype = dbrettype(H->link, colno+1);
		data = dbretdata(H->link, colno+1);
		data_len = dbretlen(H->link, colno+1);
	} else {
		coltype = dbcoltype(H->link, colno+1);
		data = dbdata(H->link, colno+1);
		data_len = dbdatlen(H->link, colno+1);
	}

	if (data_len != 0 || data != NULL) {
		if (pdo_dblib_stmt_should_stringify_col(stmt, coltype) && dbwillconvert(coltype, SQLCHAR)) {
			pdo_dblib_stmt_stringify_col(coltype, data, data_len, &zv);
		}

		if (!zv) {
			switch (coltype) {
				case SQLCHAR:
				case SQLVARCHAR:
				case SQLTEXT: {
#if ilia_0
					while (data_len>0 && data[data_len-1] == ' ') { /* nuke trailing whitespace */
						data_len--;
					}
#endif
				}
				case SQLVARBINARY:
				case SQLBINARY:
				case SQLIMAGE: {
					zv = emalloc(sizeof(zval));
					ZVAL_STRINGL(zv, (DBCHAR *) data, data_len);

					break;
				}
#ifdef SQLMSDATETIME2
				case SQLMSDATETIME2:
#endif
				case SQLDATETIME:
				case SQLDATETIM4: {
					size_t dl;
					DBDATEREC di;
					DBDATEREC dt;

					dbconvert(H->link, coltype, data, -1, SQLDATETIME, (LPBYTE) &dt, -1);
					dbdatecrack(H->link, &di, (DBDATETIME *) &dt);

					dl = spprintf(&tmp_data, 20, "%04d-%02d-%02d %02d:%02d:%02d",
#if defined(PHP_DBLIB_IS_MSSQL) || defined(MSDBLIB)
							di.year,     di.month,       di.day,        di.hour,     di.minute,     di.second
#else
							di.dateyear, di.datemonth+1, di.datedmonth, di.datehour, di.dateminute, di.datesecond
#endif
					);

					zv = emalloc(sizeof(zval));
					ZVAL_STRINGL(zv, tmp_data, dl);

					efree(tmp_data);

					break;
				}
				case SQLFLT4: {
					zv = emalloc(sizeof(zval));
					ZVAL_DOUBLE(zv, *(DBFLT4 *) data);

					break;
				}
				case SQLFLT8: {
					zv = emalloc(sizeof(zval));
					ZVAL_DOUBLE(zv, *(DBFLT8 *) data);

					break;
				}
				case SQLINT8: {
					zv = emalloc(sizeof(zval));
					ZVAL_LONG(zv, *(DBBIGINT *) data);

					break;
				}
				case SQLINT4: {
					zv = emalloc(sizeof(zval));
					ZVAL_LONG(zv, *(DBINT *) data);

					break;
				}
				case SQLINT2: {
					zv = emalloc(sizeof(zval));
					ZVAL_LONG(zv, *(DBSMALLINT *) data);

					break;
				}
				case SQLINT1:
				case SQLBIT: {
					zv = emalloc(sizeof(zval));
					ZVAL_LONG(zv, *(DBTINYINT *) data);

					break;
				}
				case SQLDECIMAL:
				case SQLNUMERIC:
				case SQLMONEY:
				case SQLMONEY4:
				case SQLMONEYN: {
					DBFLT8 float_value;
					dbconvert(NULL, coltype, data, 8, SQLFLT8, (LPBYTE) &float_value, -1);

					zv = emalloc(sizeof(zval));
					ZVAL_DOUBLE(zv, float_value);

					break;
				}

				case SQLUNIQUE: {
					if (H->stringify_uniqueidentifier) {
						/* 36-char hex string representation */
						tmp_data_len = 36;
						tmp_data = safe_emalloc(tmp_data_len, sizeof(char), 1);
						data_len = dbconvert(NULL, SQLUNIQUE, data, data_len, SQLCHAR, (LPBYTE) tmp_data, tmp_data_len);
						php_strtoupper(tmp_data, data_len);
						zv = emalloc(sizeof(zval));
						ZVAL_STRINGL(zv, tmp_data, data_len);
						efree(tmp_data);
					} else {
						/* 16-byte binary representation */
						zv = emalloc(sizeof(zval));
						ZVAL_STRINGL(zv, (DBCHAR *) data, 16);
					}
					break;
				}

				default: {
					if (dbwillconvert(coltype, SQLCHAR)) {
						pdo_dblib_stmt_stringify_col(coltype, data, data_len, &zv);
					}

					break;
				}
			}
		}
	}

	if (zv != NULL) {
		*ptr = (char*)zv;
		*len = sizeof(zval);
	} else {
		*ptr = NULL;
		*len = 0;
	}

	return 1;
}

static int pdo_dblib_stmt_get_col(pdo_stmt_t *stmt, int colno, char **ptr,
	 zend_ulong *len, int *caller_frees)
{
	*caller_frees = 1;
	return pdo_dblib_stmt_get_col_or_ret(stmt, colno, ptr, len, 0);
}

static int pdo_dblib_stmt_get_column_meta(pdo_stmt_t *stmt, zend_long colno, zval *return_value)
{
	pdo_dblib_stmt *S = (pdo_dblib_stmt*)stmt->driver_data;
	pdo_dblib_db_handle *H = S->H;
	DBTYPEINFO* dbtypeinfo;
	int coltype;

	if(colno >= stmt->column_count || colno < 0)  {
		return FAILURE;
	}

	array_init(return_value);

	dbtypeinfo = dbcoltypeinfo(H->link, colno+1);

	if(!dbtypeinfo) return FAILURE;

	coltype = dbcoltype(H->link, colno+1);

	add_assoc_long(return_value, "max_length", dbcollen(H->link, colno+1) );
	add_assoc_long(return_value, "precision", (int) dbtypeinfo->precision );
	add_assoc_long(return_value, "scale", (int) dbtypeinfo->scale );
	add_assoc_string(return_value, "column_source", dbcolsource(H->link, colno+1));
	add_assoc_string(return_value, "native_type", pdo_dblib_get_field_name(coltype));
	add_assoc_long(return_value, "native_type_id", coltype);
	add_assoc_long(return_value, "native_usertype_id", dbcolutype(H->link, colno+1));

	switch (coltype) {
		case SQLBIT:
		case SQLINT1:
		case SQLINT2:
		case SQLINT4:
			add_assoc_long(return_value, "pdo_type", PDO_PARAM_INT);
			break;
		default:
			add_assoc_long(return_value, "pdo_type", PDO_PARAM_STR);
			break;
	}

	return 1;
}

static int pdo_dblib_rpc_param_hook(pdo_stmt_t *stmt, struct pdo_bound_param_data *param,
	enum pdo_param_event event_type, int init_post)
{
	pdo_dblib_stmt *S = (pdo_dblib_stmt*)stmt->driver_data;
	pdo_dblib_db_handle *H = S->H;
	pdo_dblib_rpc_stmt *rpc = S->rpc;
	pdo_dblib_param *P = (pdo_dblib_param*)param->driver_data;
	zval *parameter;
	char *param_name = NULL;

	/* FREE: free driver_data */
	if (event_type == PDO_PARAM_EVT_FREE) {
		if (P) {
			efree(P);
		}
		return 1;
	}

	if (!Z_ISREF(param->parameter)) {
		parameter = &param->parameter;
	} else {
		parameter = Z_REFVAL(param->parameter);
	}

	if (param->paramno == -1) {
		param_name = ZSTR_VAL(param->name);
	}


	/* NORMALIZE: init/check param */
	if (event_type == PDO_PARAM_EVT_NORMALIZE) {
		/* force "@" prefix instead of ":" */
		if (param_name) {
			param_name[0] = '@';
		}

		return 1;
	}


	/* ALLOC: init driver_data (+ sort?) */
	if (event_type == PDO_PARAM_EVT_ALLOC) {

		P = emalloc(sizeof(*P));
		param->driver_data = P;
		P->output = 0;
		P->retval = 0;

		if ((param->param_type & PDO_PARAM_INPUT_OUTPUT) == PDO_PARAM_INPUT_OUTPUT) {
			P->output = 1;

			if (param_name && !strncmp(param_name, "@RETVAL", sizeof("@RETVAL")-1)) {
				P->retval = 1;
			}
		}

		/* done in rpc_exec, before INIT_POST
		zend_hash_sort(stmt->bound_params, pdo_dblib_rpc_param_cmp, 0);
		*/

		return 1;
	}


	/* EXEC_PRE: prepare data */
	if (event_type == PDO_PARAM_EVT_EXEC_PRE && !init_post) {
		long zendtype = 0, pdotype = 0;

		if (P->retval) {
			return 1;
		}

		/* set zendtype and convert value, according to the pdotype hint */
		pdotype = PDO_PARAM_TYPE(param->param_type);
		zendtype = Z_TYPE_P(parameter);
		if (pdotype == PDO_PARAM_NULL || zendtype == IS_NULL) {
			zendtype = IS_NULL;
		} else if (pdotype == PDO_PARAM_BOOL || zendtype == IS_FALSE || zendtype == IS_TRUE) {
			convert_to_long_ex(parameter);
			zendtype = _IS_BOOL;
		} else if (pdotype == PDO_PARAM_INT) {
			if (zendtype != IS_DOUBLE && zendtype != IS_LONG) {
				convert_to_long_ex(parameter);
				zendtype = IS_LONG;
			}
		} else if (pdotype != PDO_PARAM_ZVAL && zendtype != IS_STRING) {
			/* default = STRING */
			convert_to_string_ex(parameter);
			zendtype = IS_STRING;
		}

		/* set rpc bind data */
		switch(zendtype) {
			case IS_NULL:
				P->valuelen = 0;
				P->value = NULL;
				P->type = SQLVARCHAR;
				break;
			case IS_FALSE:
			case IS_TRUE:
			case _IS_BOOL:
				P->valuelen = -1;
				P->value = (LPBYTE)(&Z_LVAL_P(parameter));
				P->type = SQLINT1;
				break;
			case IS_LONG:
				P->valuelen = -1;
				P->value = (LPBYTE)(&Z_LVAL_P(parameter));
				/* TODO: smaller if possible */
				P->type = SQLINT8;
				break;
			case IS_DOUBLE:
				P->valuelen = -1;
				P->value = (LPBYTE)(&Z_DVAL_P(parameter));
				P->type = SQLFLT8;
				break;
			case IS_STRING:
				P->valuelen = Z_STRLEN_P(parameter);
				P->value = Z_STRVAL_P(parameter);
				if (P->valuelen > 8000) {
					if (P->output && dbtds(H->link) < DBTDS_7_2) {
						php_error_docref(NULL, E_WARNING, "Falling back to varchar(8000)");
						P->type = SQLVARCHAR;
					} else {
						P->type = SQLTEXT;
					}
				} else {
					P->type = SQLVARCHAR;
				}
				break;
			/* TODO */
			case IS_OBJECT:
			case IS_RESOURCE:
			case IS_ARRAY:
			default:
				pdo_raise_impl_error(stmt->dbh, stmt, "HY000", "PDO_DBLIB: RPC: Unsupported variable type.");
				return 0;
		}

		return 1;
	}


	/* INIT_POST: call dbrpcparam */
	/*
	 * dbrpcinit and dbrpcparam must be called must be called before each execution
	 * instead of calling dbrpcparam in EXEC_PRE we trigger another event in exec, after calling dbrpcinit
	 */
	if (event_type == PDO_PARAM_EVT_EXEC_PRE && init_post) {
		int status = 0;
		long maxlen = -1;

		if (P->retval) {
			return 1;
		}

		if (P->output) {
			status = DBRPCRETURN;
			P->return_pos = rpc->return_count++;
			maxlen = P->valuelen > 8000 ? P->valuelen : 8000;
		}

		if (FAIL == dbrpcparam(H->link, param_name, (BYTE)status, P->type, maxlen, P->valuelen, P->value)) {
			pdo_raise_impl_error(stmt->dbh, stmt, "HY000", "PDO_DBLIB: RPC: Unable to set parameter.");

			return 0;
		}

		return 1;
	}


	/* FETCH/EXEC: get return value */
	if (
		/* after FETCH with results / EXEC without */
		(event_type == PDO_PARAM_EVT_EXEC_POST || event_type == PDO_PARAM_EVT_FETCH_POST) &&
		(param->param_type & PDO_PARAM_INPUT_OUTPUT) == PDO_PARAM_INPUT_OUTPUT
	) {
		/* get RETVAL */
		if (P->retval) {
			if (dbhasretstat(H->link)) {
				convert_to_long_ex(parameter);
				Z_LVAL_P(parameter) = dbretstatus(H->link);
			}

			return 1;
		}

		/* fetch value from returns */
		int num_rets;
		char *value = NULL;
		size_t value_len = 0;

		if (!(num_rets = dbnumrets(H->link))) {
			return 1;
		}

		if (P->return_pos >= num_rets) {
			pdo_raise_impl_error(stmt->dbh, stmt, "HY000", "PDO_DBLIB: RPC: Missing output parameter.");
			return 0;
		}

		/* not worth checking
		if (param_name && strcmp(param_name, dbretname(H->link, P->return_pos+1))) {
			pdo_raise_impl_error(stmt->dbh, stmt, "HY000", "PDO_DBLIB: RPC: Output param name error.");
			return 0;
		} */

		pdo_dblib_stmt_get_col_or_ret(stmt, P->return_pos, &value, &value_len, 1);

		if (value && value_len == sizeof(zval)) {
			ZVAL_COPY_VALUE(parameter, (zval *)value);
		} else {
			ZVAL_NULL(parameter);
		}
		efree(value);

		return 1;
	}

	return 1;
};


static int pdo_dblib_rpc_execsql_build_stmt(pdo_stmt_t *stmt)
{
	char *query = stmt->query_string;
	size_t len = stmt->query_stringlen;
	char *query_fix = emalloc(len);
	char *tmp;
	int quote = 0;
	int positional = 0;
	int numlen;

	strncpy(query_fix, query, len);

	for (size_t i=0; i<len; i++) {
		if (query_fix[i] == '\'') {
			quote = !quote;
			continue;
		}
		if (!quote && query_fix[i] == ':') {
			query_fix[i] = '@';
			continue;
		}
		if (!quote && query_fix[i] == '?') {
			tmp = emalloc(len + 10 + 1);
			memcpy(tmp, query_fix, i);
			numlen = sprintf(tmp + i, "@%d", ++positional);
			memcpy(tmp + i + numlen, query_fix + i + 1, len - i - 1);
			len += numlen - 1;
			i += numlen - 1;
			efree(query_fix);
			query_fix = tmp;
		}
	}

	stmt->active_query_string = query_fix;
	stmt->active_query_stringlen = len;

	return 1;
}

static int pdo_dblib_rpc_execsql_build_params(pdo_stmt_t *stmt, struct pdo_bound_param_data *param)
{
	pdo_dblib_stmt *S = (pdo_dblib_stmt*)stmt->driver_data;
	pdo_dblib_rpc_stmt *rpc = S->rpc;
	pdo_dblib_param *P = (pdo_dblib_param*)param->driver_data;
	char *name, *type, *buf, *tmp;
	size_t namelen, typelen, buflen;

	if (P->retval) {
		return 1;
	}

	if (param->paramno != -1) {
		namelen = spprintf(&name, 10, "@%d", param->paramno + 1);
	} else {
		name = ZSTR_VAL(param->name);
		namelen = ZSTR_LEN(param->name);
	}

	/* prepare string: ...,@param varchar(8000) out */
	switch(P->type) {
		case SQLFLT8:
			type = " float";
			break;
		case SQLINT8:
			type = " int";
			break;
		case SQLBIT:
			type = " bit";
			break;
		case SQLTEXT:
			type = " text";
			break;
		default:
			type = " varchar(8000)";
	}
	typelen = strlen(type);

	buflen = rpc->paramslen + namelen + typelen;
	if (rpc->paramslen) buflen++;
	if (P->output) buflen += 4;
	tmp = buf = emalloc(buflen + 1);

	if (rpc->paramslen) {
		DBLIB_MEMCPY_WALK(tmp, rpc->params, rpc->paramslen);
		DBLIB_MEMCPY_WALK(tmp, ",", 1);
		efree(rpc->params);
	}
	DBLIB_MEMCPY_WALK(tmp, name, namelen);
	DBLIB_MEMCPY_WALK(tmp, type, typelen);
	if (P->output) {
		DBLIB_MEMCPY_WALK(tmp, " out", 4);
	}
	*tmp = 0;

	rpc->params = buf;
	rpc->paramslen = buflen;

	if (param->paramno != -1) {
		efree(name);
	}

	return 1;
}

static int pdo_dblib_rpc_execsql_bind(pdo_stmt_t *stmt)
{
	pdo_dblib_stmt *S = (pdo_dblib_stmt*)stmt->driver_data;
	pdo_dblib_rpc_stmt *rpc = S->rpc;
	struct pdo_bound_param_data *param;
	struct pdo_bound_param_data prm;
	pdo_dblib_param data;

	/* init temp param */
	memset(&prm, 0, sizeof(prm));
	prm.driver_data = &data;
	data.type = SQLVARCHAR;
	data.output = 0;
	data.retval = 0;

	/* prepare & bind "statement" */
	if (!stmt->executed) {
		pdo_dblib_rpc_execsql_build_stmt(stmt);
	}
	data.value = (LPBYTE)stmt->active_query_string;
	data.valuelen = stmt->active_query_stringlen;
	if (!pdo_dblib_rpc_param_hook(stmt, &prm, PDO_PARAM_EVT_EXEC_PRE, 1)) {
		return 0;
	}

	/* prepare & bind "params" */
	if (!stmt->bound_params) {
		return 1;
	}

	if (rpc->params) {
		efree(rpc->params);
		rpc->params = NULL;
	}
	rpc->paramslen = 0;
	ZEND_HASH_FOREACH_PTR(stmt->bound_params, param) {
		pdo_dblib_rpc_execsql_build_params(stmt, param);
	} ZEND_HASH_FOREACH_END();

	data.value = (LPBYTE)rpc->params;
	data.valuelen = rpc->paramslen;
	if (!pdo_dblib_rpc_param_hook(stmt, &prm, PDO_PARAM_EVT_EXEC_PRE, 1)) {
		return 0;
	}

	return 1;
}

static int pdo_dblib_rpc_param_cmp(const void* a, const void* b)
{
	/* sort by paramno, pushing -1 at the end */
	int a_no = ((struct pdo_bound_param_data *)Z_PTR(((Bucket *)a)->val))->paramno;
	int b_no = ((struct pdo_bound_param_data *)Z_PTR(((Bucket *)b)->val))->paramno;

	if (a_no == b_no) return 0;
	if (a_no == -1) return 1;
	if (b_no == -1) return -1;
	if (a_no < b_no) return -1;
	return 1;
}

static int pdo_dblib_rpc_execute(pdo_stmt_t *stmt)
{
	pdo_dblib_stmt *S = (pdo_dblib_stmt*)stmt->driver_data;
	pdo_dblib_db_handle *H = S->H;
	pdo_dblib_rpc_stmt *rpc = S->rpc;
	RETCODE ret;
	struct pdo_bound_param_data *param;
	char *sql = stmt->query_string;

	if (rpc->execsql) {
		sql = "sp_executesql";
	}

	/* rpc init */
	/* need to call DBRPCRESET on error before dbrpcexec */
	if (FAIL == dbrpcinit(H->link, sql, 0)) {
		pdo_raise_impl_error(stmt->dbh, stmt, "HY000", "PDO_DBLIB: RPC: Unable to init.");
		return 0;
	}

	/* sort params */
	if (stmt->bound_params) {
		zend_hash_sort(stmt->bound_params, pdo_dblib_rpc_param_cmp, 0);
	}

	/* bind execsql auto params */
	if (rpc->execsql && !pdo_dblib_rpc_execsql_bind(stmt)) {
		dbrpcinit(H->link, "", DBRPCRESET);
		return 0;
	}

	/* rpc bind */
	rpc->return_count = 0;
	if (stmt->bound_params) {
		ZEND_HASH_FOREACH_PTR(stmt->bound_params, param) {
			if (!pdo_dblib_rpc_param_hook(stmt, param, PDO_PARAM_EVT_EXEC_PRE, 1)) {
				dbrpcinit(H->link, "", DBRPCRESET);
				return 0;
			}
		} ZEND_HASH_FOREACH_END();
	}

	/* rpc exec */
	ret = dbrpcexec(H->link);
	if (FAIL == ret || FAIL == dbsqlok(H->link)) {
		if (FAIL == ret) {
			dbcancel(H->link);
		}

		return 0;
	}

	return 1;
}

static int pdo_dblib_stmt_param_hook(pdo_stmt_t *stmt, struct pdo_bound_param_data *param,
	enum pdo_param_event event_type)
{
	if (event_type == PDO_PARAM_EVT_FETCH_PRE) {
		return 1;
	}

	pdo_dblib_stmt *S = (pdo_dblib_stmt*)stmt->driver_data;

	if (!S->rpc) {
		return 1;
	}

	if (PDO_PARAM_TYPE(param->param_type) == PDO_PARAM_STMT) {
		return 0;
	}

	return pdo_dblib_rpc_param_hook(stmt, param, event_type, 0);
}

static int pdo_dblib_stmt_set_attr(pdo_stmt_t *stmt, zend_long attr, zval *val)
{
	pdo_dblib_stmt *S = (pdo_dblib_stmt*)stmt->driver_data;
	pdo_dblib_rpc_stmt *rpc = S->rpc;

	switch(attr) {
		/* must be set at prepare time */
		case PDO_DBLIB_ATTR_RPC:
			return 0;
		case PDO_DBLIB_ATTR_RPC_SKIP_RESULTS:
			if (rpc) {
				rpc->skip_results = zval_is_true(val);
				return 1;
			}
			return 0;
		default:
			return 0;
	}
}

static int pdo_dblib_stmt_get_attr(pdo_stmt_t *stmt, zend_long attr, zval *return_value)
{
	pdo_dblib_stmt *S = (pdo_dblib_stmt*)stmt->driver_data;
	pdo_dblib_rpc_stmt *rpc = S->rpc;

	switch(attr) {
		case PDO_DBLIB_ATTR_RPC:
			ZVAL_BOOL(return_value, rpc);
			return 1;
		case PDO_DBLIB_ATTR_RPC_SKIP_RESULTS:
			ZVAL_BOOL(return_value, rpc ? rpc->skip_results : 0);
			return 1;
		default:
			return 0;
	}
}

struct pdo_stmt_methods dblib_stmt_methods = {
	pdo_dblib_stmt_dtor,
	pdo_dblib_stmt_execute,
	pdo_dblib_stmt_fetch,
	pdo_dblib_stmt_describe,
	pdo_dblib_stmt_get_col,
	pdo_dblib_stmt_param_hook,
	pdo_dblib_stmt_set_attr,
	pdo_dblib_stmt_get_attr,
	pdo_dblib_stmt_get_column_meta, /* meta */
	pdo_dblib_stmt_next_rowset, /* nextrow */
	pdo_dblib_stmt_cursor_closer
};

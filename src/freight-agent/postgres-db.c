/*********************************************************
 * *Copyright (C) 2015 Neil Horman
 * *This program is free software; you can redistribute it and\or modify
 * *it under the terms of the GNU General Public License as published 
 * *by the Free Software Foundation; either version 2 of the License,
 * *or  any later version.
 * *
 * *This program is distributed in the hope that it will be useful,
 * *but WITHOUT ANY WARRANTY; without even the implied warranty of
 * *MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * *GNU General Public License for more details.
 * *
 * *File: postgres-db.c
 * *
 * *Author:Neil Horman
 * *
 * *Date:
 * *
 * *Description implements access to postgres db 
 * *********************************************************/
#include <stdlib.h>
#include <stdio.h>
#include <libpq-fe.h>
#include <string.h>
#include <freight-common.h>
#include <freight-db.h>


struct postgres_info {
	PGconn *conn;
};

static int pg_send_raw_sql(const char *sql,
		           const struct agent_config *acfg);

static int pg_init(struct agent_config *acfg)
{
	struct postgres_info *info = calloc(1, sizeof(struct postgres_info));
	if (!info)
		return -ENOMEM;
	acfg->db.db_priv = info;
	return 0;
}

static void pg_cleanup(struct agent_config *acfg)
{
	struct postgres_info *info = acfg->db.db_priv;

	free(info);
	acfg->db.db_priv = NULL;
	return;
}

static int pg_disconnect(struct agent_config *acfg)
{
	struct postgres_info *info = acfg->db.db_priv;
	PQfinish(info->conn);
	return 0;
}

static int pg_connect(struct agent_config *acfg)
{
	int rc = -ENOTCONN;
	char *port = "5432";

	struct postgres_info *info = acfg->db.db_priv;

	if (acfg->db.hostport)
		port = acfg->db.hostport;

	char * k[] = {
		"hostaddr",
		"port",
		"dbname",
		"user",
		"password",
		NULL
	};
	const char *const * keywords = (const char * const *)k;

	char *v[] = {
		acfg->db.hostaddr,
		port,
		acfg->db.dbname,
		acfg->db.user,
		acfg->db.password,
		NULL
	};

	const char *const * values = (const char * const *)v;

	info->conn = PQconnectdbParams(keywords, values, 0);

	if (PQstatus(info->conn) == CONNECTION_OK)
		LOG(INFO, "freight-agent connection...Established!\n");
	else {
		LOG(INFO, "freight-agent connection...Failed: %s\n",
			PQerrorMessage(info->conn));
		pg_disconnect(acfg);
		goto out;
	}	
	rc = 0;

out:
	return rc;
}

static int pg_table_op(enum table_op op, enum db_table tbl, const struct colvallist *setlist,
		       const struct colvallist *filter, const struct agent_config *acfg)
{
	const char *tblname = get_tablename(tbl);
	char *sql;
	int i, rc;

	switch (op) {

	case OP_INSERT:
		sql = strjoin("INSERT INTO ", tblname, " VALUES (", NULL);
		break;
	case OP_DELETE:
		sql = strjoin("DELETE FROM ", tblname, " WHERE ", NULL);
		break;
	case OP_UPDATE:
		sql = strjoin("UPDATE ", tblname, " SET ", NULL);
		if (setlist->count > 1)
			sql = strappend(sql, "(", NULL);
		break;
	default:
		LOG(ERROR, "Unknown table operation\n");
		return -ENOENT;
	}

	switch (op) {

	case OP_UPDATE:
		/* FALLTHROUGH */
	case OP_INSERT:
		/*
		 * Note, this expects entries to be in column order!
		 */
		for(i=0; i < setlist->count; i++) { 
			if (op == OP_UPDATE)
				sql = strappend(sql, get_colname(tbl, setlist->entries[i].column), " = ", NULL);
			if (setlist->entries[i].value)
				sql = strappend(sql, "'", setlist->entries[i].value, "'", NULL);
			else
				sql = strappend(sql, "null", NULL);
			if (i != setlist->count-1)
				sql = strappend(sql, ", ", NULL);
		}
		if ((op == OP_INSERT) && (setlist->count > 1))
			sql = strappend(sql, ")", NULL);
		if (op == OP_UPDATE)
			sql = strappend(sql, " WHERE ", NULL);
		else
			break;

		/* FALLTHROUGH AGAIN FOR OP_UPDATE */
	case OP_DELETE:
		for(i=0; i < filter->count; i++) {
			if (filter->entries[i].column == COL_VERBATIM)
				sql = strappend(sql, filter->entries[i].value, NULL);
			else
				sql = strappend(sql, get_colname(tbl, filter->entries[i].column),
						"='", filter->entries[i].value, "'", NULL);
			if (i < filter->count-1)
				sql = strappend(sql, " AND ", NULL);
		}
		break;
	default:
		break;
	}

	rc = pg_send_raw_sql(sql, acfg);

	free(sql);
	return rc;
}

static int pg_send_raw_sql(const char *sql,
		           const struct agent_config *acfg)
{
	struct postgres_info *info = acfg->db.db_priv;
	PGresult *result;
	ExecStatusType rc;
	int retc = -EINVAL;

	result = PQexec(info->conn, sql);

	rc = PQresultStatus(result);
	if (rc != PGRES_COMMAND_OK) {
		LOG(ERROR, "Unable to execute sql: %s\n",
			PQresultErrorMessage(result));
		goto out;
	}

	retc = 0;
out:
	PQclear(result);
	return retc;
}

static struct tbl* pg_get_table(enum db_table type,
			 const char *cols,
			 const char *filter,
			 const struct agent_config *acfg)
{
	struct postgres_info *info = acfg->db.db_priv;
	PGresult *result;
	ExecStatusType rc;
	int row, col, r, c;
	char *sql;
	const char *table; 
	struct tbl *rtable = NULL;

	table = get_tablename(type);
	if (filter)
		sql = strjoina("SELECT ", cols, " FROM ", table, " WHERE ", filter, NULL);
	else
		sql = strjoina("SELECT ", cols, " FROM ", table, NULL);
	result = PQexec(info->conn, sql);

	rc = PQresultStatus(result);
	if (rc != PGRES_TUPLES_OK) {
		LOG(ERROR, "Unable to query %s table: %s\n",
			table, PQresultErrorMessage(result));
		goto out;
	}

	row = PQntuples(result);
	col = PQnfields(result);

	rtable = alloc_tbl(row, col, type);
	if (!rtable)
		goto out_clear;
	
	/*
 	 * Loop through the result and copy out the table data
 	 * Column 0 is the repo name
 	 * Column 1 is the repo url
 	 */
	for (r = 0; r < row; r++) { 
		for (c = 0; c < col; c++) {
			char *tmp = PQgetvalue(result, r, c);
			if (tmp)
				rtable->value[r][c] = strdup(tmp);
			if (!rtable->value[r][c]) {
				free_tbl(rtable);
				rtable = NULL;
				goto out_clear;
			}
		}
	}


out_clear:
	PQclear(result);
out:		
	return rtable;
}

static enum event_rc pg_poll_notify(const struct agent_config *acfg)
{
	int         sock;
        fd_set      input_mask;
	PGnotify    *notify;
	enum event_rc ev_rc;
	int rc = 0;
	struct postgres_info *info = acfg->db.db_priv;

	sock = PQsocket(info->conn);

	for(;;) {
		FD_ZERO(&input_mask);
		FD_SET(sock, &input_mask);
		if (select(sock + 1, &input_mask, NULL, NULL, NULL) < 0)
		{
			/*
			 * getting EINTR is how we end this loop properly
			 */
			if (errno != EINTR)
				LOG(ERROR, "select() failed: %s\n", strerror(errno));
			rc = errno;
			break;
		}
		/* Now check for input */
		PQconsumeInput(info->conn);
		notify = PQnotifies(info->conn);
		/* Dispatch the notification here */
		ev_rc = event_dispatch(notify->relname, notify->extra);
		if (ev_rc != EVENT_CONSUMED)
			LOG(ERROR, "EVENT was not properly consumed\n");

		PQfreemem(notify);
	}

	return rc;
}

static int pg_notify(enum notify_type type, enum listen_channel chn,
		     const char *name, const struct agent_config *acfg)
{
	char *sql = strjoina("NOTIFY \"", name, "\"", NULL);

	return pg_send_raw_sql(sql, acfg);
}


static int pg_subscribe(const char *lcmd, const char *chnl, const struct agent_config *acfg)
{
	char *sql = strjoina(lcmd, " ", chnl, NULL);

        return pg_send_raw_sql(sql, acfg);
}

struct db_api postgres_db_api = {
	.init = pg_init,
	.cleanup = pg_cleanup,
	.connect = pg_connect,
	.disconnect = pg_disconnect,
	.table_op = pg_table_op,
	.send_raw_sql = pg_send_raw_sql,
	.get_table = pg_get_table,
	.notify = pg_notify,
	.subscribe = pg_subscribe,
	.poll_notify = pg_poll_notify,
};

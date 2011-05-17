
/*
 * Copyright 2011 Jeff Garzik
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "autotools-config.h"

#ifdef HAVE_MYSQL

#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include "server.h"

#include <mysql.h>

#define DEFAULT_STMT_PWDB \
	"SELECT id, password FROM pool_worker WHERE username = ?"
#define DEFAULT_STMT_SHARELOG \
	"INSERT INTO shares (userid, status, rem_host, solution)"		\
	"VALUES(?,?,?,?)"

static void bind_instr(MYSQL_BIND *bind_param, unsigned long *bind_lengths,
		       unsigned int idx, const char *s)
{
	if (s) {
		bind_param[idx].buffer_type = MYSQL_TYPE_STRING;
		bind_param[idx].buffer = (char *) s;
		bind_lengths[idx] =
		bind_param[idx].buffer_length = strlen(s);
		bind_param[idx].length = &bind_lengths[idx];
	} else {
		bind_param[idx].buffer_type = MYSQL_TYPE_NULL;
		bind_param[idx].length = &bind_lengths[idx];
	}
}

static void bind_inint(MYSQL_BIND *bind_param, unsigned long *bind_lengths,
		       unsigned int idx, const unsigned int i)
{
	bind_param[idx].buffer_type = MYSQL_TYPE_LONG;
	bind_param[idx].buffer = (void *) &i;
	bind_lengths[idx] =
	bind_param[idx].buffer_length = 0;
	bind_param[idx].length = 0;
}

static char *my_pwdb_lookup(const char *user, unsigned int *userid_out)
{
	MYSQL *db = srv.db_cxn;
	MYSQL_STMT *stmt;
	MYSQL_BIND bind_param[1], bind_res[2];
	unsigned long bind_lengths[1], bind_res_lengths[1];
	char password[256], *pass_ret;
	int pass_len;
	const char *step = "init";

	stmt = mysql_stmt_init(db);
	if (!stmt)
		return NULL;

	step = "prep";
	if (mysql_stmt_prepare(stmt, srv.db_stmt_pwdb,
			       strlen(srv.db_stmt_pwdb)))
		goto err_out;

	if (mysql_stmt_param_count(stmt))
	{
		memset(bind_param, 0, sizeof(bind_param));
		memset(bind_lengths, 0, sizeof(bind_lengths));
		bind_instr(bind_param, bind_lengths, 0, user);

		step = "bind-param";
		if (mysql_stmt_bind_param(stmt, bind_param))
			goto err_out;
	}

	memset(bind_res, 0, sizeof(bind_res));
	memset(bind_res_lengths, 0, sizeof(bind_res_lengths));

	bind_res[0].buffer_type = MYSQL_TYPE_LONG;
	bind_res[0].buffer = userid_out;
	bind_res[0].is_unsigned = true;

	bind_res[1].buffer_type = MYSQL_TYPE_STRING;
	bind_res[1].buffer = password;
	bind_res[1].buffer_length = sizeof(password);
	bind_res[1].length = &bind_res_lengths[0];

	step = "execute";
	if (mysql_stmt_execute(stmt))
		goto err_out;

	step = "bind-result";
	if (mysql_stmt_bind_result(stmt, bind_res))
		goto err_out;

	step = "store-result";
	if (mysql_stmt_store_result(stmt))
		goto err_out;

	step = "fetch";
	if (mysql_stmt_fetch(stmt))
		goto err_out;

	pass_len = bind_res_lengths[0];

	step = "malloc";
	pass_ret = malloc(pass_len + 1);
	if (!pass_ret)
		goto err_out;

	memcpy(pass_ret, password, pass_len);
	pass_ret[pass_len] = 0;

	mysql_stmt_close(stmt);
	return pass_ret;

err_out:
	mysql_stmt_close(stmt);

	applog(LOG_ERR, "mysql pwdb query failed at %s", step);
	userid_out = 0;
	return NULL;
}

static bool my_sharelog(const unsigned int status, const char *rem_host, const unsigned int userid, const char *solution)
{
	MYSQL *db = srv.db_cxn;
	MYSQL_STMT *stmt;
	MYSQL_BIND bind_param[4];
	unsigned long bind_lengths[4];
	bool rc = false;
	const char *step = "init";

	stmt = mysql_stmt_init(db);
	if (!stmt)
		return false;

	memset(bind_param, 0, sizeof(bind_param));
	memset(bind_lengths, 0, sizeof(bind_lengths));
	bind_inint(bind_param, bind_lengths, 0, userid);

	char tmp[8];
	snprintf(tmp, 8, "%u", status);
	bind_instr(bind_param, bind_lengths, 1, tmp); // bug workaround

//	bind_inint(bind_param, bind_lengths, 1, status);

	bind_instr(bind_param, bind_lengths, 2, rem_host);
	bind_instr(bind_param, bind_lengths, 3, solution);

	step = "prep";
	if (mysql_stmt_prepare(stmt, srv.db_stmt_sharelog,
			       strlen(srv.db_stmt_sharelog)))
		goto err_out;

	step = "bind-param";
	if (mysql_stmt_bind_param(stmt, bind_param))
		goto err_out;

	step = "execute";
	if (mysql_stmt_execute(stmt))
		goto err_out;

	rc = true;

out:
	mysql_stmt_close(stmt);
	return rc;

err_out:
	applog(LOG_ERR, "mysql sharelog failed at %s", step);
	goto out;
}

static bool my_open(void)
{
	MYSQL *db;
	my_bool reconnect = 1;

	if (mysql_library_init(0, NULL, NULL))
		goto err_out;

	db = mysql_init(NULL);
	if (!db)
		goto err_out_lib;

	mysql_ssl_set(db, NULL, NULL, NULL, NULL, NULL);
	mysql_options(db, MYSQL_OPT_RECONNECT, &reconnect);
	mysql_options(db, MYSQL_OPT_COMPRESS, NULL);

	if (!mysql_real_connect(db, srv.db_host, srv.db_username,
				srv.db_password, srv.db_name,
				srv.db_port > 0 ? srv.db_port : 0,
				NULL, 0))
		goto err_out_db;

	srv.db_cxn = db;
	if (srv.db_stmt_pwdb == NULL || !*srv.db_stmt_pwdb)
		srv.db_stmt_pwdb = strdup(DEFAULT_STMT_PWDB);
	if (srv.db_stmt_sharelog == NULL || !*srv.db_stmt_sharelog)
		srv.db_stmt_sharelog = strdup(DEFAULT_STMT_SHARELOG);
	return true;

err_out_db:
	mysql_close(db);
err_out_lib:
	mysql_library_end();
err_out:
	return false;
}

static void my_close(void)
{
	MYSQL *db = srv.db_cxn;

	mysql_close(db);
	mysql_library_end();
}

struct server_db_ops mysql_db_ops = {
	.pwdb_lookup	= my_pwdb_lookup,
	.sharelog	= my_sharelog,
	.open		= my_open,
	.close		= my_close,
};

#endif /* HAVE_MYSQL */
